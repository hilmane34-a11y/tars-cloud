#include "lengan.h"
#include <Wire.h>
#include <PCF8574.h>

#define PCF_ADDR 0x20

PCF8574 pcf(PCF_ADDR);

// ===============================
// PCF8574 -> L298N
// ===============================

// Channel A = MOTOR LENGAN
#define L_IN1 0   // P0
#define L_IN2 1   // P1

// Channel B = MOTOR GRIPER
#define G_IN1 2   // P2
#define G_IN2 3   // P3

void lenganInit()
{
    pcf.begin();

    pcf.pinMode(L_IN1, OUTPUT);
    pcf.pinMode(L_IN2, OUTPUT);

    pcf.pinMode(G_IN1, OUTPUT);
    pcf.pinMode(G_IN2, OUTPUT);

    tanganStop();
}

// ===============================
// LENGAN
// ===============================

void lenganNaik()
{
    pcf.digitalWrite(L_IN1, HIGH);
    pcf.digitalWrite(L_IN2, LOW);
}

void lenganTurun()
{
    pcf.digitalWrite(L_IN1, LOW);
    pcf.digitalWrite(L_IN2, HIGH);
}

void lenganStop()
{
    pcf.digitalWrite(L_IN1, LOW);
    pcf.digitalWrite(L_IN2, LOW);
}

// ===============================
// GRIPER
// ===============================

void griperBuka()
{
    pcf.digitalWrite(G_IN1, HIGH);
    pcf.digitalWrite(G_IN2, LOW);
}

void griperTutup()
{
    pcf.digitalWrite(G_IN1, LOW);
    pcf.digitalWrite(G_IN2, HIGH);
}

void griperStop()
{
    pcf.digitalWrite(G_IN1, LOW);
    pcf.digitalWrite(G_IN2, LOW);
}

// ===============================
// STOP SEMUA
// ===============================

void tanganStop()
{
    lenganStop();
    griperStop();
}
