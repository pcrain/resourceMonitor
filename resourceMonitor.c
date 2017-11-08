#include <stdio.h>    //fscanf()
#include <stdlib.h>   //malloc()
#include <unistd.h>   //sleep()
#include <sys/time.h> //gettimeofday()
#include <time.h>     //time()
#include <signal.h>   //signal()

// #define DEBUGIT  //Comment out for release

#define POLLSECONDS      1  //Time between polls
#define FLUSHRATE        60 //Flush every FLUSHRATE polls
#define IDEVICE          "wlp1s0:"
#define LOGFILE          "/home/pretzel/documents/logs/resmon.tsv"

#define TEMPERATURE_FILE "/sys/class/thermal/thermal_zone3/temp"
#define FAN_FILE         "/sys/class/hwmon/hwmon3/fan1_input"
#define AC_FILE          "/sys/class/power_supply/AC/online"
#define BAT_FULL_FILE    "/sys/class/power_supply/BAT0/charge_full"
#define BAT_NOW_FILE     "/sys/class/power_supply/BAT0/charge_now"
#define BAT_CURRENT_FILE "/sys/class/power_supply/BAT0/current_now"
#define POWER_FILE       "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj"
#define DISK_FILE        "/sys/block/sda/stat"
#define DISK_SCAN        "%*llu %*llu %llu %*llu %*llu %*llu %llu"
#define INET_SCAN        "%*[^\n]\n%*[^\n]\nwlp1s0: %llu %*llu %*llu %*llu %*llu %*llu %*llu %*llu %llu"

#define BSIZE            16

#ifdef DEBUGIT
  #define CLEAR() printf("\033[H\033[J")
  #define dprintf(...) printf(__VA_ARGS__)
#else
  #define CLEAR() {}
  #define dprintf(...) {}
#endif

#define SCAN(F,S,...) {file = fopen((F),"r"); fscanf(file,(S),__VA_ARGS__); fclose(file);}
#define ASNUM(x)      ((x)-'0')
#define ISNUM(x)      ((x) >= '0' && (x) <= '9')
#define EXISTS(F)     (access((F), F_OK ) != -1)

static char BUFFER[BSIZE];     //Generic character buffer
static FILE* OFILE;
static volatile int RUNNING = 1;

static inline unsigned long long utime() {
  struct timeval t;
  gettimeofday(&t, 0);
  return ((unsigned long)time(NULL)*1000000)+t.tv_usec;
}

static inline long long unsigned read_uint_string(const char* fname) {
  long lSize;
  for (unsigned i = 0; i < BSIZE; ++i)
    BUFFER[i] = ' ';

  FILE* pFile = fopen (fname , "r");
  fseek (pFile , 0 , SEEK_END);
  lSize = ftell (pFile);
  if (lSize > BSIZE)
    lSize = BSIZE;
  rewind (pFile);

  fread (BUFFER,1,lSize,pFile);
  fclose (pFile);
  long long unsigned val = 0;
  for (unsigned i = 0; i < lSize && ISNUM(BUFFER[i]); ++i)
    val = val*10+ASNUM(BUFFER[i]);
  return val;
}

static inline long long unsigned read_two_chars_as_uint(const char* fname) {
  FILE* pFile = fopen (fname , "r");
  fread (BUFFER,1,2,pFile);
  fclose (pFile);
  return ASNUM(BUFFER[0])*10+ASNUM(BUFFER[1]);
}

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

int main(int argc, char** argv) {
  #ifndef DEBUGIT
    printf("Resource monitoring started\n");
  #endif

  unsigned long long logBeginTime = 0;

  unsigned counter = 0;
  unsigned long long lastUser, lastLow, lastSys, lastIdle, lastPower, lastSent, lastRecv, lastRead, lastWrite;
  FILE* file;
  int hasHeader = EXISTS(LOGFILE);
  OFILE = fopen (LOGFILE , "a");
  if (!hasHeader) {
    logBeginTime = utime()/1000000;
    fprintf(OFILE, "%llu\tram\ttemp\tfan\tcharge\tdrain\tdown\tup\tread\twrite\tpower\tcpu\n",logBeginTime);
  } else {
    logBeginTime = read_uint_string(LOGFILE);
    fprintf(OFILE, "\t\t\t\t\t\t\t\t\t\t\t\n");
  }
  unsigned long long startTime;

  //Initialize time-based data
    SCAN("/proc/stat","cpu %llu %llu %llu %llu", &lastUser, &lastLow, &lastSys, &lastIdle);
    lastPower = read_uint_string(POWER_FILE);
    SCAN("/proc/net/dev",INET_SCAN, &lastRecv, &lastSent);
    SCAN(DISK_FILE,DISK_SCAN, &lastRead, &lastWrite);

  signal(SIGINT, intHandler);
  while(RUNNING) {
    startTime = utime(); sleep(POLLSECONDS); CLEAR(); ++counter; //Refresh
    //Time
      dprintf("Time:        %4.2llu us\n",startTime);
      fprintf(OFILE,"%0.1llu\t",(startTime/1000000)-logBeginTime);

    //RAM
      unsigned long long memTotal, memFree;
      SCAN("/proc/meminfo","MemTotal: %llu kB\nMemFree: %*llu kB\nMemAvailable: %llu kB\n",&memTotal,&memFree)
      dprintf("RAM Usage:   %4.2f%%\n",100.0f*(float)(memTotal-memFree)/memTotal);
      fprintf(OFILE,"%4.2f\t",100.0f*(float)(memTotal-memFree)/memTotal);

    //Temperature
      unsigned temperature = read_two_chars_as_uint(TEMPERATURE_FILE);
      dprintf("CPU Temp:    %u C\n",temperature);
      fprintf(OFILE,"%u\t",temperature);

    //Fan
      unsigned fan = read_uint_string(FAN_FILE);
      dprintf("Fan Speed:   %u RPM\n",fan);
      fprintf(OFILE,"%u\t",fan);

    //Battery
      unsigned long long bat_full, bat_now, bat_curr;
      bat_full = read_uint_string(BAT_FULL_FILE);
      bat_now = read_uint_string(BAT_NOW_FILE);
      bat_curr = read_uint_string(BAT_CURRENT_FILE)/1000;
      dprintf("Bat Charge:  %4.2f%%\n",100*(float)bat_now/bat_full);
      fprintf(OFILE,"%4.2f\t",100*(float)bat_now/bat_full);
      dprintf("Bat Current: %u mJ\n",bat_curr);
      fprintf(OFILE,"%u\t",bat_curr);

    //Internet Speed
      unsigned long long recv, sent;
      SCAN("/proc/net/dev",INET_SCAN, &recv, &sent);
      dprintf("Download:    %4.2f bytes/sec\n",1000000.0f*(recv - lastRecv)/(utime()-startTime));
      fprintf(OFILE,"%4.2f\t",1000000.0f*(recv - lastRecv)/(utime()-startTime));
      dprintf("Upload:      %4.2f bytes/sec\n",1000000.0f*(sent - lastSent)/(utime()-startTime));
      fprintf(OFILE,"%4.2f\t",1000000.0f*(sent - lastSent)/(utime()-startTime));
      lastRecv = recv; lastSent = sent;

    //Disk Speed
      unsigned long long read, write;
      SCAN(DISK_FILE,DISK_SCAN, &read, &write);
      dprintf("Disk Read:   %4.2f bytes/sec\n",1000000.0f*(read - lastRead)/(utime()-startTime));
      fprintf(OFILE,"%4.2f\t",1000000.0f*(read - lastRead)/(utime()-startTime));
      dprintf("Disk Write:  %4.2f bytes/sec\n",1000000.0f*(write - lastWrite)/(utime()-startTime));
      fprintf(OFILE,"%4.2f\t",1000000.0f*(write - lastWrite)/(utime()-startTime));
      lastRead = read; lastWrite = write;

    //Power
      unsigned long long power;
      // SCAN(POWER_FILE,"%llu", &power);
      power = read_uint_string(POWER_FILE);
      dprintf("Power Draw:  %4.2f Watts\n",((float)(power - lastPower))/(utime()-startTime));  //In uJ / uS == J / s == W
      fprintf(OFILE,"%4.2f\t",((float)(power - lastPower))/(utime()-startTime));
      lastPower = power;

    //CPU
      unsigned long long totalUser, totalUserLow, totalSys, totalIdle, total;
      SCAN("/proc/stat", "cpu %llu %llu %llu %llu", &totalUser, &totalUserLow, &totalSys, &totalIdle);
      total = (totalUser + totalUserLow + totalSys) - (lastUser + lastLow + lastSys);
      dprintf("CPU Usage:   %4.2f%%\n",((float)total/(total+(totalIdle-lastIdle)))*100);
      fprintf(OFILE,"%4.2f\t",((float)total/(total+(totalIdle-lastIdle)))*100);
      lastUser = totalUser; lastLow = totalUserLow; lastSys = totalSys; lastIdle = totalIdle;

    fprintf(OFILE,"\n");
    if (!(counter % FLUSHRATE))
      fflush(OFILE);
  }
}
