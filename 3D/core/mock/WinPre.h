#pragma once

// ============================================================================
// mock/WinPre.h
//
// 强制包含（cl /FI）用的前置头。
//
// 为什么需要：
//   src\util\Data.h 里的 FGameData 用了 DWORD / FVector，这些来自
//   windows.h + ue4math。原工程靠 ext\DMALibrary\pch.h 间接带进来；
//   mock 不走那套 pch，所以这里显式补上，避免去改任何原文件。
//
//   NOMINMAX 必须在 windows.h 之前定义，否则 min/max 宏会撞坏
//   std::min / std::max 调用（本工程大量使用）。
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
