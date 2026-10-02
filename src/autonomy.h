#pragma once
#include "env.h"

void autonomyBegin();
void autonomyStop();
void autonomySetEnvironment(const EnvState &environment);
void autonomyUpdate(bool enabled, bool busy);
bool autonomyIsMoving();
