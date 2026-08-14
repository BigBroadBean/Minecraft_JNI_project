//============================================================================
//  swapstub.dll —— 假客户端测试辅助库 (V65 hook 版冒烟测试用)
//
//  作用: 假客户端的 Java 线程 ("Client thread", 模拟游戏渲染线程) 每 10ms
//  调用 Java_TestSwapClient_swap, 本函数内调用真实的 gdi32!SwapBuffers ——
//  被注入的 MCCombatStatusJni.dll 钩住的就是这个函数, 从而触发完整的
//  "渲染帧内 GetEnv 复用 JNIEnv" 链路 (不新建线程、不 Attach)。
//
//  编译 (build.bat 已包含):
//    g++ -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ -s \
//        -I..\..\..\include -I..\..\..\include\win32 \
//        -o swapstub.dll swapstub.cpp -lgdi32
//============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <jni.h>

// 注意: 必须 extern "C" —— 否则 C++ 名字修饰导致 JVM 按符号名
// Java_TestSwapClient_swap 查找失败 (UnsatisfiedLinkError)。
extern "C" {
JNIEXPORT void JNICALL Java_TestSwapClient_swap(JNIEnv* env, jclass cls)
{
    (void)env;
    (void)cls;
    // 控制台窗口 DC 不是双缓冲 GL 表面, SwapBuffers 会返回 FALSE ——
    // 这无关紧要: 钩子链在进入真实函数前就已触发, 我们只借这条调用路径。
    HWND w = GetConsoleWindow();
    HDC hdc = GetDC(w);
    SwapBuffers(hdc);
    if (hdc && w) ReleaseDC(w, hdc);
}
} // extern "C"
