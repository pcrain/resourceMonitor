#include <stdio.h>                               //fopen(), fread(), etc.
#include <unistd.h>                              //sleep(), read();
#include <sys/time.h>                            //gettimeofday()
#include <time.h>                                //time()
#include <signal.h>                              //signal()
#include <sys/resource.h>                        //setpriority()

#include <fcntl.h>                               //open
#include <errno.h>
#include <stdlib.h>

// #define DEBUGIT                               //Comment out for release

//User configurables
#define POLLSECONDS   1                          //Seconds between polls
#define FLUSHRATE     60                         //Polls between writes to disk
#define IDEVICE       "wlp1s0"                   //Primary internet interface
#define DRIVE         "sda"                      //Primary hard drive
#define USER          "pretzel"                  //Name of login user
#define LOGFILE       "/home/"USER"/.reslog.tsv" //Location to save log file

//File locations for various assets
#define TEMP_FILE     "/sys/class/thermal/thermal_zone3/temp"
#define FAN_FILE      "/sys/class/hwmon/hwmon3/fan1_input"
#define AC_FILE       "/sys/class/power_supply/AC/online"
#define BAT_FULL_FILE "/sys/class/power_supply/BAT0/charge_full"
#define BAT_NOW_FILE  "/sys/class/power_supply/BAT0/charge_now"
#define CURRENT_FILE  "/sys/class/power_supply/BAT0/current_now"
#define POWER_FILE    "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj"
#define DISK_FILE     "/sys/block/"DRIVE"/stat"
#define NET_DN_FILE   "/sys/class/net/"IDEVICE"/statistics/rx_bytes"
#define NET_UP_FILE   "/sys/class/net/"IDEVICE"/statistics/tx_bytes"

//Header / blank string definitions for log file
#define HEADSTRING    \
  "%llu\tram\ttemp\tfan\tcharge\tdrain\tdown\tup\tread\twrite\tpower\tcpu\n"
#define BLANKSTRING   "\t\t\t\t\t\t\t\t\t\t\t\n"

//Useful constants and macros
#define BATTPOLL      10/POLLSECONDS             //Rate to poll battery info
#define MAXUINT       10000000000                //Max size of uints in LOGFILE
#define BSIZE         256                        //Size of file read buffer
#define ASNUM(x)      ((x)-'0')                  //Convert char to number
#define ISNUM(x)      ((x)>='0' && (x)<='9')     //Check if a char is a numeral
#define EXISTS(F)     (access((F),F_OK)!=-1)     //Check if a file exists
#define TIMEBEG       u64 _t = utime();          //Begin and end a timer
#define TIMEEND       printf("Took %llu us\n",utime()-_t);

//Typedefs and other miscellaneous globals
typedef unsigned long long u64;                  //Typedef for 64-bit uint
static FILE*          OFILE;                     //Log file handle
static char           BUFFER[BSIZE];             //Generic character buffer
static char           CACHE[10001][2][8];        //Cached strings
static char           CLENS[10001][2];           //Cached string lengths
static struct timeval T;                         //Buffer for holding time data
static volatile int   RUNNING = 1;               //Whether we're still running

//Sigint handler to nicely close the log file
static inline void intHandler(int dummy) {
  printf("Closing nicely...\n");
  RUNNING = 0;
  fflush(OFILE); fclose(OFILE);
}

//Get a microsecond resolution timestamp
static inline u64 utime() {
  gettimeofday(&T, 0);
  return ((u64)time(NULL)*1000000)+T.tv_usec;
}

//Read a single uint from a file
static inline u64 read_uint_string(FILE* f) {
  rewind(f);
  fread (BUFFER, 1, 16, f);
  u64 val = 0;
  for (unsigned i = 0; ISNUM(BUFFER[i]); ++i)
    val = val*10+ASNUM(BUFFER[i]);
  return val;
}

//Read the first uint in a file, trim the last t digits, and log it immediately
static inline void read_and_write_chars(FILE* f, char t) {
  rewind(f);
  fread (BUFFER, 1, 16, f);
  unsigned char nbytes;
  for (nbytes = 0; ISNUM(BUFFER[nbytes]); ++nbytes);
  fwrite (BUFFER, 1, nbytes-t, OFILE);
  fwrite ("\t", 1, 1, OFILE);
}

//Find the nth integer in a file and return it
static inline u64 read_nth_uint_string(FILE* f, unsigned n) {
  unsigned c        = 0;        //Counter for current uint we're in
  unsigned in_uint  = 0;        //Whether we're in a uint
  unsigned found    = 0;        //Whether we're in the nth uint
  unsigned val      = 0;        //Value to return
  unsigned start    = ftell(f); //Starting byte of the file

  fread (BUFFER, 1, BSIZE, f);
  for(unsigned i = 0; i < BSIZE; ++i) {
    if (ISNUM(BUFFER[i])) {            //If the character is a number
      if (found) {
        val = val*10+ASNUM(BUFFER[i]); //If it's the nth number, keep reading
      } else if (!in_uint) {
        if ((++c) == n) {
          found = 1;
          val = ASNUM(BUFFER[i]);
        }
        in_uint = 1;
      }
    } else {
      if (found) {
        fseek(f, start+i, SEEK_SET);   //Rewind file to just after the number
        return val;
      }
      in_uint = 0;
    }
  }

  return 0;
}

//Write a uint to the log file
static inline void write_uint(u64 n, unsigned dplaces) {
  if (n <= 10000) { //Write an already-formatted string if we're already cached
    fwrite (&(CACHE[n][dplaces >> 1][0]) , 1, CLENS[n][dplaces >> 1], OFILE);
    return;
  }
  unsigned char unit, pos = 0;
  for (u64 e = MAXUINT; e > 0; e /= 10) {
    if (n < e) {
      if (pos == 0)
        continue;
      BUFFER[pos] = '0';
    } else {
      unit = (n/e);
      BUFFER[pos] = unit+'0';
      n -= unit*e;
    }
    ++pos;
  }
  BUFFER[pos] = '\t';
  fwrite (BUFFER , 1, pos+1, OFILE);
  return;
}

//Doubles are trivially truncated to two decimal places and saved like uints
#define write_double(x) write_uint(100*(x),2)

//Prepare a cache of strings for integers < 10000 and floats < 100
static inline void prepare_cache() {
  //Cache int strings
  unsigned c = 0;
  for(unsigned t = 0; t < 10; ++t) {
    for(unsigned h = 0; h < 10; ++h) {
      for(unsigned e = 0; e < 10; ++e) {
        for(unsigned o = 0; o < 10; ++o) {
          if (!t) {
            if (!h) {
              if (!e) {
                //Single digit
                CACHE[c][0][0] = '0'+o;
                CACHE[c][0][1] = '\t';
                CLENS[c][0] = 2;
                ++c; continue;
              }
              //Two digits
              CACHE[c][0][0] = '0'+e;
              CACHE[c][0][1] = '0'+o;
              CACHE[c][0][2] = '\t';
              CLENS[c][0] = 3;
              ++c; continue;
            }
            //Three digits
            CACHE[c][0][0] = '0'+h;
            CACHE[c][0][1] = '0'+e;
            CACHE[c][0][2] = '0'+o;
            CACHE[c][0][3] = '\t';
            CLENS[c][0] = 4;
            ++c; continue;
          }
          //Four digits
          CACHE[c][0][0] = '0'+t;
          CACHE[c][0][1] = '0'+h;
          CACHE[c][0][2] = '0'+e;
          CACHE[c][0][3] = '0'+o;
          CACHE[c][0][4] = '\t';
          CLENS[c][0] = 5;
          ++c; continue;
        }
      }
    }
  }
  //Five digits
  CACHE[10000][0][0] = '1';
  CACHE[10000][0][1] = '0';
  CACHE[10000][0][2] = '0';
  CACHE[10000][0][3] = '0';
  CACHE[10000][0][4] = '0';
  CACHE[10000][0][5] = '\t';
  CLENS[10000][0] = 6;

  //Cache float strings
  c = 0;
  for(unsigned t = 0; t < 10; ++t) {
    for(unsigned h = 0; h < 10; ++h) {
      for(unsigned e = 0; e < 10; ++e) {
        for(unsigned o = 0; o < 10; ++o) {
          if (!t) {
            if (!c) {
              //Zero
              CACHE[c][1][0] = '0';
              CACHE[c][1][1] = '\t';
              CLENS[c][1] = 2;
              ++c; continue;
            }
            //Three digits
            CACHE[c][1][0] = '0'+h;
            CACHE[c][1][1] = '.';
            CACHE[c][1][2] = '0'+e;
            CACHE[c][1][3] = '0'+o;
            CACHE[c][1][4] = '\t';
            CLENS[c][1] = 5;
            ++c; continue;
          }
          //Four digits
          CACHE[c][1][0] = '0'+t;
          CACHE[c][1][1] = '0'+h;
          CACHE[c][1][2] = '.';
          CACHE[c][1][3] = '0'+e;
          CACHE[c][1][4] = '0'+o;
          CACHE[c][1][5] = '\t';
          CLENS[c][1] = 6;
          ++c; continue;
        }
      }
    }
  }
  //Five digits (100.00 == 100)
  CACHE[10000][1][0] = '1';
  CACHE[10000][1][1] = '0';
  CACHE[10000][1][2] = '0';
  CACHE[10000][1][3] = '\t';
  CLENS[10000][1] = 4;
}

int main(int argc, char** argv) {
  //Declare loop variables
    u64 lUser, lLow, lSys, lIdle, lPower, lSent, lRecv, lRead, lWrit;
    u64 startTime, logBeginTime = 0, lastTime = 0;
    unsigned counter = 0, battcount = 0;

  //Initialize log file
    if (!EXISTS(LOGFILE)) {
      logBeginTime = (u64)time(NULL);
      OFILE = fopen (LOGFILE , "a");
      fprintf(OFILE, HEADSTRING,logBeginTime);
    } else {
      OFILE = fopen (LOGFILE , "r");
      logBeginTime = read_uint_string(OFILE);
      fclose(OFILE);
      OFILE = fopen (LOGFILE , "a");
      fprintf(OFILE, BLANKSTRING);
    }

  //Initialize file handles
    #define INIT(h,f) FILE* h = fopen((f),"r"); setvbuf(h, NULL, _IONBF, 0)
    INIT(stat_file     , "/proc/stat");
    INIT(netup_file    , NET_UP_FILE);
    INIT(netdn_file    , NET_DN_FILE);
    INIT(mem_file      , "/proc/meminfo");
    INIT(temp_file     , TEMP_FILE);
    INIT(fan_file      , FAN_FILE);
    INIT(ac_file       , AC_FILE);
    INIT(bat_full_file , BAT_FULL_FILE);
    INIT(bat_now_file  , BAT_NOW_FILE);
    INIT(current_file  , CURRENT_FILE);
    INIT(power_file    , POWER_FILE);
    INIT(disk_file     , DISK_FILE);

  //Initialize time-based data
    lUser  = read_nth_uint_string(stat_file,1);
    lLow   = read_nth_uint_string(stat_file,1);
    lSys   = read_nth_uint_string(stat_file,1);
    lIdle  = read_nth_uint_string(stat_file,1);
    lRead  = read_nth_uint_string(disk_file,3);
    lWrit  = read_nth_uint_string(disk_file,4);
    lRecv  = read_uint_string(netdn_file);
    lSent  = read_uint_string(netup_file);
    lPower = read_uint_string(power_file);

  //Remember total RAM / battery capacity
    u64 memTotal  = read_nth_uint_string(mem_file,1);
    double b_rate = 100.0/read_uint_string(bat_full_file);
    u64 bat_charge = read_uint_string(bat_now_file);
    u64 bat_current = read_uint_string(current_file) / 1000;

  prepare_cache();                 //Prepare write cache
  signal(SIGINT, intHandler);      //Register signal handler
  setpriority(PRIO_PROCESS, 0, 1); //Set priority lower than normal processes

  printf("Resource monitoring started\n");

  startTime = utime();
  while(RUNNING) {  //Main log loop
    //Time
      lastTime = startTime;
      startTime = utime();
      if ((startTime-lastTime)/1000000 > 3*POLLSECONDS) {
        fprintf(OFILE, BLANKSTRING); //Add a blank line after extended suspend
      }
      sleep(POLLSECONDS);
    //RAM
      rewind(mem_file);
      u64 memFree = read_nth_uint_string(mem_file,3);

    //Battery (polling is slow, so do so at most once every 10 seconds)
      if (++battcount >= BATTPOLL) {
        //Update battery info
        bat_charge = read_uint_string(bat_now_file);
        bat_current = read_uint_string(current_file) / 1000;
        battcount = 0;
      }

    //Internet Speed
      u64 recv = read_uint_string(netdn_file);
      u64 sent = read_uint_string(netup_file);

    //Disk Speed
      rewind(disk_file);
      u64 read = read_nth_uint_string(disk_file,3);
      u64 writ = read_nth_uint_string(disk_file,4);

    //Power
      u64 power = read_uint_string(power_file);

    //CPU
      rewind(stat_file);
//BOTTLENECK (750 us)
      u64 tUser    = read_nth_uint_string(stat_file,1);
      u64 tUserLow = read_nth_uint_string(stat_file,1);
      u64 tSys     = read_nth_uint_string(stat_file,1);
      u64 tIdle    = read_nth_uint_string(stat_file,1);
//END BOTTLENECK
      u64 total    = (tUser + tUserLow + tSys)-(lUser + lLow + lSys);

    //Write everything to file
      u64 delta = (utime()-startTime);
      double scale = 1000000.0/delta;       //microsecond to second adjustment
      write_uint((startTime/1000000)-logBeginTime,0);
      write_double(100*(double)(memTotal-memFree)/memTotal);
      read_and_write_chars(temp_file,3);    //Read and write temperature/1000
//BOTTLENECK (700 us)
      read_and_write_chars(fan_file,0);     //Read and write fan speed
//END BOTTLENECK
      write_double(b_rate*bat_charge);
      write_uint(bat_current   ,0);
      write_uint(scale * (recv - lRecv)   ,0);
      write_uint(scale * (sent - lSent)   ,0);
      write_uint(scale * (read - lRead)   ,0);
      write_uint(scale * (writ - lWrit) ,0);
      write_double((double)(power - lPower)/delta);
      write_double(100*(double)total/(total+(tIdle-lIdle)));
      fwrite ("\n" , 1, 1, OFILE);

    //Debug printing
      #ifdef DEBUGIT
        printf("\033[H\033[J")  //Clear screen
        printf("Time:        %4.2llu us\n",startTime);
        printf("RAM Usage:   %4.2f%%\n",(100.0f*(memTotal-memFree))/memTotal);
        printf("CPU Temp:    %u C\n",read_uint_string(temp_file)/1000);
        printf("Fan Speed:   %u RPM\n",read_uint_string(fan_file));
        printf("Bat Charge:  %4.2f%%\n",write_double(b_rate*bat_charge));
        printf("Bat Current: %u mJ\n",read_uint_string(current_file)/1000);
        printf("Download:    %4.2f bytes/sec\n",1000000.0f*(recv-lRecv)/delta);
        printf("Upload:      %4.2f bytes/sec\n",1000000.0f*(sent-lSent)/delta);
        printf("Disk Read:   %4.2f bytes/sec\n",1000000.0f*(read-lRead)/delta);
        printf("Disk Write:  %4.2f bytes/sec\n",1000000.0f*(writ-lWrit)/delta);
        printf("Power Draw:  %4.2f Watts\n",(float)(power-lPower)/elapsed);
        printf("CPU Usage:   %4.2f%%\n",100*(float)total/(total+tIdle-lIdle));
      #endif

    //Update l variables
      lRecv  = recv; lSent = sent;
      lRead  = read; lWrit = writ;
      lPower = power;
      lUser  = tUser; lLow = tUserLow; lSys = tSys; lIdle = tIdle;

    //Flush write buffer to log file
      if (++counter == FLUSHRATE) {
        fflush(OFILE); counter = 0;
      }
  }

  //Clean up
    fclose(stat_file);
    fclose(netdn_file);
    fclose(netup_file);
    fclose(temp_file);
    fclose(fan_file);
    fclose(ac_file);
    fclose(bat_full_file);
    fclose(bat_now_file);
    fclose(current_file);
    fclose(power_file);
    fclose(disk_file);
}
