#include "deep_sleep.h"
#include <Preferences.h>
#include <time.h>
#include <esp_sleep.h>
#include <WiFi.h>

static Preferences sleepPrefs;

void deepSleepBegin(){
  sleepPrefs.begin("tars-sleep",false);
}

static int currentDayKey(){
  time_t now=time(nullptr);
  if(now<1704067200)return -1;
  struct tm t;
  localtime_r(&now,&t);
  return (t.tm_year+1900)*1000+t.tm_yday;
}

bool deepSleepDue(){
  time_t now=time(nullptr);
  if(now<1704067200)return false;
  struct tm t;
  localtime_r(&now,&t);
  return t.tm_hour>=22;
}

bool deepSleepAlarmDone(){
  int key=currentDayKey();
  return key>=0&&sleepPrefs.getInt("alarmDay",-1)==key;
}

void deepSleepMarkAlarmDone(){
  int key=currentDayKey();
  if(key>=0)sleepPrefs.putInt("alarmDay",key);
}

void deepSleepEnter(){
  time_t now=time(nullptr);
  if(now<1704067200)return;

  struct tm t;
  localtime_r(&now,&t);

  int secondsNow=t.tm_hour*3600+t.tm_min*60+t.tm_sec;
  int secondsWake=5*3600+30*60;
  int secondsSleep=secondsWake-secondsNow;

  if(secondsSleep<=0)secondsSleep+=86400;

  Serial.printf("TARS: DEEP SLEEP %lu MENIT\n",
                (unsigned long)(secondsSleep/60));

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);

  esp_sleep_enable_timer_wakeup(
    (uint64_t)secondsSleep*1000000ULL
  );
  Serial.flush();
  esp_deep_sleep_start();
}
