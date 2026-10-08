#include "lengan.h"
#include <Wire.h>
#include <PCF8574.h>

#define PCF_ADDR 0x20

PCF8574 pcf(PCF_ADDR);

// ===============================
// PCF8574 -> L298N
// ===============================

// Channel A = MOTOR LENGAN
#define L_IN1 0
#define L_IN2 1
#define L_EN  2

// Channel B = MOTOR GRIPER
#define G_IN1 3
#define G_IN2 4
#define G_EN  5

void lenganInit()
{
    pcf.begin();

    pcf.pinMode(L_IN1, OUTPUT);
    pcf.pinMode(L_IN2, OUTPUT);
    pcf.pinMode(L_EN, OUTPUT);

    pcf.pinMode(G_IN1, OUTPUT);
    pcf.pinMode(G_IN2, OUTPUT);
    pcf.pinMode(G_EN, OUTPUT);

    tanganStop();
}

// ===============================
// LENGAN
// ===============================

void lenganNaik()
{
    pcf.digitalWrite(L_IN1, HIGH);
    pcf.digitalWrite(L_IN2, LOW);
    pcf.digitalWrite(L_EN, HIGH);
}

void lenganTurun()
{
    pcf.digitalWrite(L_IN1, LOW);
    pcf.digitalWrite(L_IN2, HIGH);
    pcf.digitalWrite(L_EN, HIGH);
}

void lenganStop()
{
    pcf.digitalWrite(L_IN1, LOW);
    pcf.digitalWrite(L_IN2, LOW);
    pcf.digitalWrite(L_EN, LOW);
}

// ===============================
// GRIPER
// ===============================

void griperBuka()
{
    pcf.digitalWrite(G_IN1, HIGH);
    pcf.digitalWrite(G_IN2, LOW);
    pcf.digitalWrite(G_EN, HIGH);
}

void griperTutup()
{
    pcf.digitalWrite(G_IN1, LOW);
    pcf.digitalWrite(G_IN2, HIGH);
    pcf.digitalWrite(G_EN, HIGH);
}

void griperStop()
{
    pcf.digitalWrite(G_IN1, LOW);
    pcf.digitalWrite(G_IN2, LOW);
    pcf.digitalWrite(G_EN, LOW);
}

// ===============================
// STOP SEMUA
// ===============================

void tanganStop()
{
    lenganStop();
    griperStop();
}
