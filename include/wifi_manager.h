#pragma once

bool wifiManagerBegin();
bool wifiManagerConnect(bool requireTime = false);
void wifiManagerDisconnect();
