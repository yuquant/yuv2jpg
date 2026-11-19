#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>

// 启动图形界面（内部会创建窗口并进入消息循环）
int RunGui(HINSTANCE hInst, int nCmdShow);
