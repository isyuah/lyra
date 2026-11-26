#pragma once

#include "tasks.h"

struct LyraNativeAsync;
using LyraNativeSubmit = bool (*)(LyraNativeAsync*, void*);
using LyraNativeCancel = void (*)(void*);

LyraTask* lyraTaskNative(LyraNativeSubmit submit, LyraNativeCancel cancel, void* data, void* rejected = nullptr);
bool lyraNativeTryComplete(LyraNativeAsync* operation, void* boxed, LyraTask* cleanup = nullptr);
void lyraNativeComplete(LyraNativeAsync* operation, void* boxed = nullptr);
void lyraNativeKeepCleanup(LyraTask* cleanup);
