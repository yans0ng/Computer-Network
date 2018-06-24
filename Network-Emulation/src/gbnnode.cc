#include <common.h>
#include <setjmp.h>
#include <signal.h>
#include <thread>
#include <atomic>
#include <string>
#include <queue>

#define GBNNODE_DEBUG 0 // Debugging mode
////////////////////////////////////////////////////////////////////////////////
// Move to gbnnode.h
////////////////////////////////////////////////////////////////////////////////
const size_t BUFSIZE = 2048;
// Shared memory accross threads
bool proc_running = true;

std::atomic_flag msglock = ATOMIC_FLAG_INIT; // lock for "messages"
std::queue<std::string> messages;

std::atomic_flag acklock = ATOMIC_FLAG_INIT; // lock for ACKs
size_t ack;



static int fd;
static struct sockaddr_in src, dest;

inline void __grab_lock(std::atomic_flag& lock) {
  while (lock.test_and_set(std::memory_order_acquire));
}

inline void __release_lock(std::atomic_flag& lock) {
  lock.clear(std::memory_order_release);
}

inline void __send(const std::string& msg) {
  if (sendto(fd, msg.c_str(), msg.size(), 0, (struct sockaddr *)&dest,
      sizeof(dest)) < 0) {
    std::cerr << "error: failed while sending " << msg << std::endl;
    std::cerr << "error: " << strerror(errno) << std::endl;
    throw std::exception();
  }
}
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

// The user input thread
// Takes in command line input and process command type
void cin_proc() {
  while (proc_running) {
    std::string line;
    std::getline(std::cin, line);

    size_t delim = line.find(' ');
    if (line.substr(0, delim) != "send") {
      #if GBNNODE_DEBUG
      std::cout << "warning: support only send operation" << std::endl;
      #endif
      continue;
    }

    std::string message = line.substr(delim+1, line.size()-delim);

    #if GBNNODE_DEBUG
    std::cout << "cin_proc: got " << message << std::endl;
    #endif
    __grab_lock(msglock);
    messages.push(message);
    __release_lock(msglock);
  }
}

// Packet-dropping manager
static bool dropmode; // 1 for deterministic, 0 for probabilistic
static int dropsum;
static int dropn;
static float dropprob;
inline float rand_float() {
  return static_cast<float>(rand()) / RAND_MAX;
}
bool drop_packet() {
  return (dropmode) ? (++dropsum % dropn) == 0 : rand_float() > dropprob;
}

// The go-back-n protocol thread
void __timeout_handler(int signo);
void __gbn_fsm(const std::string& buf, const int& window);
// Pops out the top message and send it over UDP
void gbn_proc(const int& window) {
  while (proc_running) {
    if (messages.empty()) continue;
    __grab_lock(msglock);
    std::string buf = messages.front();
    #if GBNNODE_DEBUG
    std::cout << "gbn_proc: send " << buf << std::endl;
    #endif
    messages.pop();
    __release_lock(msglock);
    __gbn_fsm(buf, window);
    std::string msg = "END";
    __send(msg);
    std::cout << "[Summary] " << "packets dropped, loss rate = ??"
        << std::endl;
  }
}

bool __timeout_flag;
void __timeout_handler(int signo) {
  MY_INFO_STREAM << "timeout" << std::endl;
  //longjmp(env, ETIME); // timeout errno
  __timeout_flag = true;
}

// Implementation of GBN Finite State Machine
void __gbn_fsm(const std::string& buf, const int& window) {
  size_t base = 0, nextseq = 0;
  struct sigaction sa;
  sa.sa_handler = &__timeout_handler;
  sigaction (SIGALRM, &sa, 0);
  __timeout_flag = false;
  ualarm(500000, 0);
  //int val = setjmp(env);
  // NOTE: longjmp cannot parse val 0 to setjmp. 0 is for the first run.
send_state:
  if (__timeout_flag) goto timeout_state;
  for (; nextseq < base + window -1 && nextseq < buf.size(); nextseq++) {
    std::string msg = "SEQ:" + std::to_string(nextseq) + ":" + buf[nextseq];
    __send(msg);
    MY_INFO_STREAM << "packet" << nextseq << ' ' << buf[nextseq] << " sent"
        << std::endl;
  }
chkack_state:
  __grab_lock(acklock);
  base = ack + 1;
  if (base == buf.size()) {
    __release_lock(acklock);
    ualarm(0, 0); // on Linux, setting zero will cancel the alarm
    return;
  } else if (base == nextseq) {
    __release_lock(acklock);
    ualarm(500000, 0);
    //longjmp(env, 0);
    goto send_state;
  }
  __release_lock(acklock);
  goto send_state;
timeout_state:
  #if GBNNODE_DEBUG
  std::cout << "resend " << base << " to " << nextseq << std::endl;
  #endif
  for (size_t i = base; i < nextseq && i < buf.size(); i++) {
    std::string msg = "SEQ:" + std::to_string(i) + ":" + buf[i];
    __send(msg);
    MY_INFO_STREAM << "packet" << i << ' ' << buf[i] << " sent" << std::endl;
    #if GBNNODE_DEBUG
    std::cout << "resend " << msg << std::endl;
    #endif
  }
  ualarm(500000, 0);
  __timeout_flag = false;
  goto chkack_state;

  #if GBNNODE_DEBUG
  std::cout << "__gbn_fsm: something went wrong... shouldn't be here"
      << std::endl;
  #endif
  ualarm(0, 0); 
  proc_running = false;
}

// The main thread listens to the bound port
void recv_proc() {
  int expected = 0;
  struct sockaddr_in rcvaddr;
  socklen_t addrlen = sizeof(rcvaddr);
  char buf[BUFSIZE];
  while (proc_running) {
    //ssize_t ret = recvfrom(fd, buf, BUFSIZE, 0, (struct sockaddr*)&rcvaddr,
    //    &addrlen);
    ssize_t ret;
    while ((ret = recvfrom(fd, buf, BUFSIZE, 0, (struct sockaddr*)&rcvaddr,
            &addrlen)) == -1 && errno == EINTR);
    //if (errno == EINTR) {
    //  std::cout << "ignore alarm siganl, call recvfrom again." << std::endl;
    //  continue;
    //}
    if (ret > 0) {
      buf[ret] = 0;
      #if GBNNODE_DEBUG
      std::cout << "recv_proc: receive message - " << buf << std::endl;
      #endif
      
      std::string inmsg(buf, ret);
      size_t pos0 = inmsg.find(':');
      if (inmsg.substr(0, pos0) == "ACK") {
        size_t pos1 = inmsg.find(':', pos0+1);
        int newack = stoi(inmsg.substr(pos0+1, pos1-pos0-1));
        MY_INFO_STREAM << "ACK" << newack << " received, window moved to "
            << newack+1 << std::endl;
        __grab_lock(acklock);
        ack = newack;
        __release_lock(acklock);
      } else if (inmsg.substr(0, pos0) == "SEQ"){
        size_t pos1 = inmsg.find(':', pos0+1);
        int seq = stoi(inmsg.substr(pos0+1, pos1-pos0-1));
        if (drop_packet()) {
          MY_INFO_STREAM << "packet" << seq << ' ' << inmsg[pos1+1]
              << " discarded" << std::endl;
        } else if (seq == expected) {
          
          MY_INFO_STREAM << "packet " << seq << ' ' << inmsg[pos1+1]
              << " received" << std::endl;
          std::string outmsg = "ACK:" + std::to_string(expected);
          __send(outmsg);
          MY_INFO_STREAM << "ACK" << expected << " sent, expecting " 
              << expected+1 << std::endl;
          expected++;
        } else {
          MY_INFO_STREAM << "packet " << seq << ' ' << inmsg[pos1+1]
              << " received" << std::endl;
          std::string outmsg = "ACK:" + std::to_string(expected-1);
          __send(outmsg);
          MY_INFO_STREAM << "ACK" << expected-1 << " sent, expecting "
              << expected << std::endl;
        }

      } else if (inmsg == "END") {
        expected = 0;
        std::cout << "[Summary] " << "packets discarded, loss rate = ??"
            << std::endl;
      }
    } else {
      std::cerr << "error: recvfrom failed" << std::endl;
      std::cerr << "error: " <<  strerror(errno) << std::endl;
      //throw std::exception();
    }
  }
}

int main(int argc, char** argv) {
  #if GBNNODE_DEBUG
  std::cout << "Arguments\n" << "Window size = " << std::stoi(argv[1])
      << std::endl << "Source port: " << cstr_to_port(argv[2]) << std::endl
      << "Destination port: " << cstr_to_port(argv[3]) << std::endl;
  #endif
  const int window = std::stoi(argv[1]);
  set_udp_addr(src, cstr_to_port(argv[2]));
  set_udp_addr(dest, cstr_to_port(argv[3]));
  if (std::string(argv[4]) == "-d") {
    dropmode = true;
    dropn = std::stoi(argv[5]);
    std::cout << "deterministic mode / " << dropn << std::endl;
  } else if (std::string(argv[4]) == "-p") {
    dropmode = false;
    dropprob = std::stof(argv[5]);
    std::cout << "probabilistic mode / " << dropprob << std::endl;
  } else {
    MY_ERROR_STREAM << "error: invalid drop mode" << std::endl;
    exit(1);
  }

  initialize(fd, src);
  std::thread user_cin(cin_proc);
  std::thread send_gbn(gbn_proc, window);
  recv_proc();
  user_cin.join();
  send_gbn.join();
  close(fd);
  return 0;
}