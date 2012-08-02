/// \file buffer.cpp
/// Contains the main code for the Buffer.

#include <fcntl.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sstream>
#include <sys/time.h>
#include <mist/config.h>
#include "buffer_stream.h"

/// Holds all code unique to the Buffer.
namespace Buffer{

  volatile bool buffer_running = true; ///< Set to false when shutting down.
  Stream * thisStream = 0;
  Socket::Server SS; ///< The server socket.

  /// Gets the current system time in milliseconds.
  long long int getNowMS(){
    timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec * 1000 + t.tv_usec/1000;
  }//getNowMS


  ///A simple signal handler that ignores all signals.
  void termination_handler (int signum){
    switch (signum){
      case SIGKILL: buffer_running = false; break;
      case SIGPIPE: return; break;
      default: return; break;
    }
  }

  void handleStats(void * empty){
    if (empty != 0){return;}
    Socket::Connection StatsSocket = Socket::Connection("/tmp/mist/statistics", true);
    while (buffer_running){
      usleep(1000000); //sleep one second
      if (!StatsSocket.connected()){
        StatsSocket = Socket::Connection("/tmp/mist/statistics", true);
      }
      if (StatsSocket.connected()){
        StatsSocket.write(Stream::get()->getStats()+"\n\n");
      }
    }
  }

  void handleUser(void * v_usr){
    user * usr = (user*)v_usr;
    std::cerr << "Thread launched for user " << usr->MyStr << ", socket number " << usr->S.getSocket() << std::endl;

    usr->myRing = thisStream->getRing();
    if (!usr->S.write(thisStream->getHeader())){
      usr->Disconnect("failed to receive the header!");
      return;
    }

    while (usr->S.connected()){
      usleep(5000); //sleep 5ms
      if (usr->S.canRead()){
        usr->inbuffer.clear();
        char charbuf;
        while ((usr->S.iread(&charbuf, 1) == 1) && charbuf != '\n' ){
          usr->inbuffer += charbuf;
        }
        if (usr->inbuffer != ""){
          if (usr->inbuffer[0] == 'P'){
            std::cout << "Push attempt from IP " << usr->inbuffer.substr(2) << std::endl;
            if (thisStream->checkWaitingIP(usr->inbuffer.substr(2))){
              if (thisStream->setInput(usr->S)){
                std::cout << "Push accepted!" << std::endl;
                usr->S = Socket::Connection(-1);
                return;
              }else{
                usr->Disconnect("Push denied - push already in progress!");
              }
            }else{
              usr->Disconnect("Push denied - invalid IP address!");
            }
          }
          if (usr->inbuffer[0] == 'S'){
            usr->tmpStats = Stats(usr->inbuffer.substr(2));
            unsigned int secs = usr->tmpStats.conntime - usr->lastStats.conntime;
            if (secs < 1){secs = 1;}
            usr->curr_up = (usr->tmpStats.up - usr->lastStats.up) / secs;
            usr->curr_down = (usr->tmpStats.down - usr->lastStats.down) / secs;
            usr->lastStats = usr->tmpStats;
            thisStream->saveStats(usr->MyStr, usr->tmpStats);
          }
        }
      }
      usr->Send();
    }
    thisStream->cleanUsers();
    std::cerr << "User " << usr->MyStr << " disconnected, socket number " << usr->S.getSocket() << std::endl;
  }

  /// Loop reading DTSC data from stdin and processing it at the correct speed.
  void handleStdin(void * empty){
    if (empty != 0){return;}
    long long int timeDiff = 0;//difference between local time and stream time
    unsigned int lastPacket = 0;//last parsed packet timestamp
    std::string inBuffer;
    char charBuffer[1024*10];
    unsigned int charCount;
    long long int now;

    while (std::cin.good() && buffer_running){
      //slow down packet receiving to real-time
      now = getNowMS();
      if ((now - timeDiff >= lastPacket) || (lastPacket - (now - timeDiff) > 5000)){
        thisStream->getWriteLock();
        if (thisStream->getStream()->parsePacket(inBuffer)){
          thisStream->getStream()->outPacket(0);
          lastPacket = thisStream->getStream()->getTime();
          if ((now - timeDiff - lastPacket) > 5000 || (now - timeDiff - lastPacket < -5000)){
            timeDiff = now - lastPacket;
          }
          thisStream->dropWriteLock(true);
        }else{
          thisStream->dropWriteLock(false);
          std::cin.read(charBuffer, 1024*10);
          charCount = std::cin.gcount();
          inBuffer.append(charBuffer, charCount);
        }
      }else{
        if ((lastPacket - (now - timeDiff)) > 999){
          usleep(999000);
        }else{
          usleep((lastPacket - (now - timeDiff)) * 1000);
        }
      }
    }
    buffer_running = false;
    SS.close();
  }

  /// Loop reading DTSC data from an IP push address.
  /// No changes to the speed are made.
  void handlePushin(void * empty){
    if (empty != 0){return;}
    std::string inBuffer;
    while (buffer_running){
      if (thisStream->getIPInput().connected()){
        if (inBuffer.size() > 0){
          thisStream->getWriteLock();
          if (thisStream->getStream()->parsePacket(inBuffer)){
            thisStream->getStream()->outPacket(0);
            thisStream->dropWriteLock(true);
          }else{
            thisStream->dropWriteLock(false);
            thisStream->getIPInput().iread(inBuffer);
            usleep(1000);//1ms wait
          }
        }else{
          thisStream->getIPInput().iread(inBuffer);
          usleep(1000);//1ms wait
        }
      }else{
        usleep(1000000);//1s wait
      }
    }
    SS.close();
  }

  /// Starts a loop, waiting for connections to send data to.
  int Start(int argc, char ** argv) {
    Util::Config conf = Util::Config(argv[0], PACKAGE_VERSION);
    conf.addOption("stream_name", JSON::fromString("{\"arg_num\":1, \"arg\":\"string\", \"help\":\"Name of the stream this buffer will be providing.\"}"));
    conf.addOption("awaiting_ip", JSON::fromString("{\"arg_num\":2, \"arg\":\"string\", \"default\":\"\", \"help\":\"IP address to expect incoming data from. This will completely disable reading from standard input if used.\"}"));
    conf.parseArgs(argc, argv);
    
    std::string name = conf.getString("stream_name");

    SS = Socket::makeStream(name);
    if (!SS.connected()) {
      perror("Could not create stream socket");
      return 1;
    }
    thisStream = Stream::get();
    thisStream->setName(name);
    Socket::Connection incoming;
    Socket::Connection std_input(fileno(stdin));

    tthread::thread StatsThread = tthread::thread(handleStats, 0);
    tthread::thread * StdinThread = 0;
    std::string await_ip = conf.getString("awaiting_ip");
    if (await_ip == ""){
      StdinThread = new tthread::thread(handleStdin, 0);
    }else{
      thisStream->setWaitingIP(await_ip);
      StdinThread = new tthread::thread(handlePushin, 0);
    }

    while (buffer_running && SS.connected()){
      //check for new connections, accept them if there are any
      //starts a thread for every accepted connection
      incoming = SS.accept(false);
      if (incoming.connected()){
        user * usr_ptr = new user(incoming);
        thisStream->addUser(usr_ptr);
        usr_ptr->Thread = new tthread::thread(handleUser, (void *)usr_ptr);
      }
    }//main loop

    // disconnect listener
    buffer_running = false;
    std::cout << "End of input file - buffer shutting down" << std::endl;
    SS.close();
    StatsThread.join();
    StdinThread->join();
    delete thisStream;
    return 0;
  }

};//Buffer namespace

/// Entry point for Buffer, simply calls Buffer::Start().
int main(int argc, char ** argv){
  return Buffer::Start(argc, argv);
}//main
