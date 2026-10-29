#pragma once
#include <Arduino.h>

#define KNOWLEDGE_WIDTH  128
#define KNOWLEDGE_HEIGHT 64
#define KNOWLEDGE_FEATURE_SIZE 64
#define KNOWLEDGE_LABEL_SIZE 24

bool knowledgeBegin();
bool knowledgeBuildFeatures(const uint16_t *image, uint8_t *features);
bool knowledgeLearnFeatures(const char *label, const uint8_t *features);
bool knowledgeLearn(const char *label, const uint16_t *image);
bool knowledgeRecognize(const uint16_t *image, char *labelOut, size_t labelOutSize, uint8_t *confidenceOut = nullptr);
bool knowledgeRecognizeFeatures(const uint8_t *features, char *labelOut, size_t labelOutSize, uint8_t *confidenceOut = nullptr);
uint16_t knowledgeSampleCount();
bool knowledgeDeleteLabel(const char *label);
bool knowledgeHasLabel(const char *label);
