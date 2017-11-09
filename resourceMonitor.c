#include <stdio.h>        //fscanf()
#include <stdlib.h>       //malloc()
#include <unistd.h>       //sleep()
#include <sys/time.h>     //gettimeofday()
#include <time.h>         //time()
#include <signal.h>       //signal()
#include <string.h>       //memcpy()
#include <sys/resource.h> //setpriority()

// #define DEBUGIT         //Comment out for release

#define MAXUINT          10000000000 //Max size uint we will be writing to the log file
#define POLLSECONDS      1           //Time between polls
#define FLUSHRATE        60          //Flush every FLUSHRATE polls
#define IDEVICE          "wlp1s0"    //Name of device connected to the internet
#define DRIVE            "sda"       //Primary hard drive on your system
#define USER             "pretzel"   //Name of login user
#define LOGFILE          "/home/"USER"/documents/logs/resmon.tsv" //Location to save the log file to

#define TEMPERATURE_FILE "/sys/class/thermal/thermal_zone3/temp"
#define FAN_FILE         "/sys/class/hwmon/hwmon3/fan1_input"
#define AC_FILE          "/sys/class/power_supply/AC/online"
#define BAT_FULL_FILE    "/sys/class/power_supply/BAT0/charge_full"
#define BAT_NOW_FILE     "/sys/class/power_supply/BAT0/charge_now"
#define BAT_CURRENT_FILE "/sys/class/power_supply/BAT0/current_now"
#define POWER_FILE       "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj"
#define DISK_FILE        "/sys/block/"DRIVE"/stat"
#define NET_DN_FILE      "/sys/class/net/"IDEVICE"/statistics/rx_bytes"
#define NET_UP_FILE      "/sys/class/net/"IDEVICE"/statistics/tx_bytes"

#define BSIZE            256

#ifdef DEBUGIT
  #define CLEAR() printf("\033[H\033[J")
  #define dprintf(...) printf(__VA_ARGS__)
#else
  #define CLEAR() {}
  #define dprintf(...) {}
#endif

#define ASNUM(x)      ((x)-'0')
#define ISNUM(x)      ((x) >= '0' && (x) <= '9')
#define EXISTS(F)     (access((F), F_OK) != -1)

static char         BUFFER[BSIZE];     //Generic character buffer
static char         LINEBUF[128];      //Line buffer for writing
static int          LBPOS = 0;
static FILE*        OFILE;
static volatile int RUNNING = 1;

static inline void intHandler(int dummy) {
  if(RUNNING) {
    RUNNING = 0;
    printf("Closing nicely...\n");
    fflush(OFILE);
    fclose(OFILE);
  } else {
    printf("Closing immediately...\n");
    exit(-10);
  }
}

static inline unsigned long long utime() {
  struct timeval t;
  gettimeofday(&t, 0);
  return ((unsigned long long)time(NULL)*1000000)+t.tv_usec;
}

static inline long long unsigned read_uint_string(FILE* f) {
  rewind(f);
  fread (BUFFER, 1, 16, f);
  long long unsigned val = 0;
  for (unsigned i = 0; ISNUM(BUFFER[i]); ++i)
    val = val*10+ASNUM(BUFFER[i]);
  return val;
}

static inline long long unsigned read_two_chars_as_uint(FILE* f) {
  rewind(f);
  fread (BUFFER,1,2,f);
  return ASNUM(BUFFER[0])*10+ASNUM(BUFFER[1]);
}

//Find the nth integer in a file and return it
static inline long long unsigned read_nth_uint_string(FILE* f, unsigned n) {
  unsigned c        = 0;
  unsigned in_uint  = 0; //Whether we're in a uint
  unsigned found    = 0;
  unsigned val      = 0;
  unsigned startpos = ftell(f);

  fread (BUFFER, 1, BSIZE, f);
  for(unsigned i = 0; i < BSIZE; ++i) {
    if (ISNUM(BUFFER[i])) {  //If the character is a number
      if (found) {
        val = val*10+ASNUM(BUFFER[i]); //If it's the nth number, keep reading it in
      } else if (!in_uint) {
        if ((++c) == n) {
          found = 1;
          val = ASNUM(BUFFER[i]);
        }
        in_uint = 1;
      }
    } else {
      if (found) {
        fseek(f, startpos+i, SEEK_SET);
        return val;  //We're done once we're past the nth number
      } else if (in_uint) {
        in_uint = 0;
      }
    }
  }

  return 0;
}

static inline void write_uint(unsigned long long n, unsigned dplaces) {
  if (n == 0) {
    LINEBUF[LBPOS] = '0';
    LINEBUF[LBPOS+1] = '\t';
    LBPOS += 2;
    return;
  }
  unsigned unit, pos = 0;
  for (unsigned long long e = MAXUINT; e > 0; e /= 10) {
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
  if (dplaces == 0) {
    memcpy(&(LINEBUF[LBPOS]),BUFFER,(pos+1));
    LBPOS += pos+1;
    return;
  }
  if (pos-dplaces > 0) {
    memcpy(&(LINEBUF[LBPOS]),BUFFER,(pos-dplaces));
    LBPOS += pos-dplaces;
  }
  LINEBUF[LBPOS] = '.';
  ++LBPOS;
  memcpy(&(LINEBUF[LBPOS]),&(BUFFER[pos-dplaces]), dplaces+1);
  LBPOS += dplaces+1;
}

#define write_double(x) write_uint(100*(x),2)

int main(int argc, char** argv) {
  #ifndef DEBUGIT
    printf("Resource monitoring started\n");
  #endif

  setpriority(PRIO_PROCESS, 0, 1); //We are slightly lower priority than normal processes

  long long unsigned logBeginTime = 0;

  unsigned counter = 0;
  unsigned long long lastUser, lastLow, lastSys, lastIdle, lastPower, lastSent, lastRecv, lastRead, lastWrite;
  int hasHeader = EXISTS(LOGFILE);
  if (!hasHeader) {
    // logBeginTime = utime()/1000000;
    logBeginTime = (unsigned long)time(NULL);
    OFILE = fopen (LOGFILE , "a");
    fprintf(OFILE, "%llu\tram\ttemp\tfan\tcharge\tdrain\tdown\tup\tread\twrite\tpower\tcpu\n",logBeginTime);
  } else {
    OFILE = fopen (LOGFILE , "r");
    logBeginTime = read_uint_string(OFILE);
    // printf("%llu\n",logBeginTime);
    fclose(OFILE);
    OFILE = fopen (LOGFILE , "a");
    fprintf(OFILE, "\t\t\t\t\t\t\t\t\t\t\t\n");
  }
  unsigned long long startTime;

  //Initialize file handles
    FILE* stat_file         = fopen("/proc/stat","r");     setvbuf(stat_file,        NULL, _IONBF, 0);
    FILE* netup_file        = fopen(NET_UP_FILE,"r");      setvbuf(netup_file,         NULL, _IONBF, 0);
    FILE* netdn_file        = fopen(NET_DN_FILE,"r");      setvbuf(netdn_file,         NULL, _IONBF, 0);
    FILE* mem_file          = fopen("/proc/meminfo","r");  setvbuf(mem_file,         NULL, _IONBF, 0);
    FILE* temperature_file  = fopen(TEMPERATURE_FILE,"r"); setvbuf(temperature_file, NULL, _IONBF, 0);
    FILE* fan_file          = fopen(FAN_FILE,"r");         setvbuf(fan_file,         NULL, _IONBF, 0);
    FILE* ac_file           = fopen(AC_FILE,"r");          setvbuf(ac_file,          NULL, _IONBF, 0);
    FILE* bat_full_file     = fopen(BAT_FULL_FILE,"r");    setvbuf(bat_full_file,    NULL, _IONBF, 0);
    FILE* bat_now_file      = fopen(BAT_NOW_FILE,"r");     setvbuf(bat_now_file,     NULL, _IONBF, 0);
    FILE* bat_current_file  = fopen(BAT_CURRENT_FILE,"r"); setvbuf(bat_current_file, NULL, _IONBF, 0);
    FILE* power_file        = fopen(POWER_FILE,"r");       setvbuf(power_file,       NULL, _IONBF, 0);
    FILE* disk_file         = fopen(DISK_FILE,"r");        setvbuf(disk_file,        NULL, _IONBF, 0);

  //Initialize time-based data
    lastUser = read_nth_uint_string(stat_file,1);
    lastLow = read_nth_uint_string(stat_file,1);
    lastSys = read_nth_uint_string(stat_file,1);
    lastIdle = read_nth_uint_string(stat_file,1);
    lastPower = read_uint_string(power_file);

    lastRecv = read_uint_string(netdn_file);
    lastSent = read_uint_string(netup_file);

    lastRead = read_nth_uint_string(disk_file,3);
    lastWrite = read_nth_uint_string(disk_file,4);

  signal(SIGINT, intHandler);  //Register signal handler
  while(RUNNING) {
    //Time
      startTime = utime(); sleep(POLLSECONDS); CLEAR(); ++counter; //Refresh

    //RAM
      unsigned long long memTotal, memFree;
      rewind(mem_file);
      memTotal = read_nth_uint_string(mem_file,1);
      memFree = read_nth_uint_string(mem_file,2);

    //Temperature
      unsigned temperature = read_two_chars_as_uint(temperature_file);

    //Fan
      unsigned fan = read_uint_string(fan_file);

    //Battery
      unsigned long long bat_full, bat_now, bat_curr;
      bat_full = read_uint_string(bat_full_file);
      bat_now  = read_uint_string(bat_now_file);
      bat_curr = read_uint_string(bat_current_file)/1000;

    //Internet Speed
      unsigned long long recv, sent;
      recv = read_uint_string(netdn_file);
      sent = read_uint_string(netup_file);

    //Disk Speed
      unsigned long long read, write;
      rewind(disk_file);
      read = read_nth_uint_string(disk_file,3);
      write = read_nth_uint_string(disk_file,4);

    //Power
      unsigned long long power;
      power = read_uint_string(power_file);

    //CPU
      unsigned long long totalUser, totalUserLow, totalSys, totalIdle, total;
      rewind(stat_file);
      totalUser    = read_nth_uint_string(stat_file,1);
      totalUserLow = read_nth_uint_string(stat_file,1);
      totalSys     = read_nth_uint_string(stat_file,1);
      totalIdle    = read_nth_uint_string(stat_file,1);
      total = (totalUser + totalUserLow + totalSys) - (lastUser + lastLow + lastSys);

    //Debug printing
      dprintf("Time:        %4.2llu us\n",startTime);
      dprintf("RAM Usage:   %4.2f%%\n",100.0f*(float)(memTotal-memFree)/memTotal);
      dprintf("CPU Temp:    %u C\n",temperature);
      dprintf("Fan Speed:   %u RPM\n",fan);
      dprintf("Bat Charge:  %4.2f%%\n",100*(float)bat_now/bat_full);
      dprintf("Bat Current: %u mJ\n",bat_curr);
      dprintf("Download:    %4.2f bytes/sec\n",1000000.0f*(recv - lastRecv)/(utime()-startTime));
      dprintf("Upload:      %4.2f bytes/sec\n",1000000.0f*(sent - lastSent)/(utime()-startTime));
      dprintf("Disk Read:   %4.2f bytes/sec\n",1000000.0f*(read - lastRead)/(utime()-startTime));
      dprintf("Disk Write:  %4.2f bytes/sec\n",1000000.0f*(write - lastWrite)/(utime()-startTime));
      dprintf("Power Draw:  %4.2f Watts\n",((float)(power - lastPower))/(utime()-startTime));  //In uJ / uS == J / s == W
      dprintf("CPU Usage:   %4.2f%%\n",((float)total/(total+(totalIdle-lastIdle)))*100);

    //Write everything to file
      unsigned long long elapsed = (utime()-startTime);
      double delta = 1000000.0/elapsed;
      LBPOS = 0;
      write_uint((startTime/1000000)-logBeginTime,0);
      write_double(100*(double)(memTotal-memFree)/memTotal);
      write_uint(temperature,0);
      write_uint(fan,0);
      write_double(100*(double)bat_now/bat_full);
      write_uint(bat_curr,0);
      write_uint(delta * (recv - lastRecv)   ,0);
      write_uint(delta * (sent - lastSent)   ,0);
      write_uint(delta * (read - lastRead)   ,0);
      write_uint(delta * (write - lastWrite) ,0);
      write_double(((double)(power - lastPower))/elapsed);
      write_double(((double)total/(total+(totalIdle-lastIdle)))*100);
      LINEBUF[LBPOS] = '\n';
      fwrite (LINEBUF , 1, LBPOS+1, OFILE);

    //Update last variables
      lastRecv  = recv; lastSent = sent;
      lastRead  = read; lastWrite = write;
      lastPower = power;
      lastUser  = totalUser; lastLow = totalUserLow; lastSys = totalSys; lastIdle = totalIdle;

    //Flush write buffer to log file
      if (!(counter % FLUSHRATE))
        fflush(OFILE);
  }

  //Clean up
    fclose(stat_file);
    fclose(netdn_file);
    fclose(netup_file);
    fclose(temperature_file);
    fclose(fan_file);
    fclose(ac_file);
    fclose(bat_full_file);
    fclose(bat_now_file);
    fclose(bat_current_file);
    fclose(power_file);
    fclose(disk_file);
}
