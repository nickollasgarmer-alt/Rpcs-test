// Etapa 1: teste de limites do Xbox Dev Mode (JIT, RAM, threads)
// Template do Visual Studio: "Core App (C++/WinRT)"
// O resultado e gravado em LocalState\resultado.txt (veja pelo Device Portal > File Explorer)

#include <windows.h>
#include <memoryapi.h>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

static std::ofstream g_log;

static void Log(const std::string& s)
{
    g_log << s << std::endl; // endl da flush: se o app cair, o log fica salvo
    OutputDebugStringA((s + "\n").c_str());
}

static std::string Err() { return " (erro " + std::to_string(GetLastError()) + ")"; }

static void TestSystemInfo()
{
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    Log("CPUs logicas: " + std::to_string(si.dwNumberOfProcessors));

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
    {
        Log("RAM fisica total (MB): " + std::to_string(ms.ullTotalPhys >> 20));
        Log("RAM fisica livre (MB): " + std::to_string(ms.ullAvailPhys >> 20));
        Log("Virtual total (MB): " + std::to_string(ms.ullTotalVirtual >> 20));
    }
}

// JIT: aloca RW, escreve codigo (mov eax,42; ret), troca para RX e executa
static void TestJit()
{
    Log("== JIT ==");
    const unsigned char code[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };

    // Tentativa 1: RW -> RX (o jeito recomendado)
    void* mem = VirtualAllocFromApp(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { Log("FALHA: alloc RW" + Err()); return; }
    memcpy(mem, code, sizeof(code));

    ULONG old = 0;
    if (!VirtualProtectFromApp(mem, 4096, PAGE_EXECUTE_READ, &old))
    {
        Log("FALHA: VirtualProtectFromApp RX" + Err() + " <- falta a capability codeGeneration?");
    }
    else
    {
        FlushInstructionCache(GetCurrentProcess(), mem, sizeof(code));
        Log("Executando codigo gerado (RW->RX)...");
        int r = reinterpret_cast<int(*)()>(mem)();
        Log(r == 42 ? "OK: JIT RW->RX funcionou (retornou 42)" : "FALHA: retorno inesperado " + std::to_string(r));
    }
    VirtualFree(mem, 0, MEM_RELEASE);

    // Tentativa 2: RWX direto
    void* rwx = VirtualAllocFromApp(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!rwx) { Log("RWX direto: nao permitido" + Err()); return; }
    memcpy(rwx, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), rwx, sizeof(code));
    Log("Executando codigo gerado (RWX)...");
    int r2 = reinterpret_cast<int(*)()>(rwx)();
    Log(r2 == 42 ? "OK: JIT RWX funcionou" : "FALHA: retorno inesperado " + std::to_string(r2));
    VirtualFree(rwx, 0, MEM_RELEASE);
}

// RAM: aloca e toca blocos de 256 MB ate falhar (limite de 32 GB para nao rodar para sempre)
static void TestRam()
{
    Log("== RAM ==");
    const SIZE_T chunk = 256ull << 20;
    std::vector<void*> blocks;
    for (int i = 1; i <= 128; ++i)
    {
        void* p = VirtualAllocFromApp(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) { Log("Alloc falhou em " + std::to_string(i * 256) + " MB" + Err()); break; }
        for (SIZE_T off = 0; off < chunk; off += 4096) static_cast<volatile char*>(p)[off] = 1;
        blocks.push_back(p);
        Log("OK: " + std::to_string(i * 256) + " MB alocados e tocados");
    }
    Log("RAM utilizavel pelo app (MB): " + std::to_string(blocks.size() * 256));
    for (void* p : blocks) VirtualFree(p, 0, MEM_RELEASE);
}

// Threads: cria ate 512 threads bloqueadas
static void TestThreads()
{
    Log("== THREADS ==");
    std::atomic<bool> stop{ false };
    std::vector<std::thread> threads;
    try
    {
        for (int i = 1; i <= 512; ++i)
        {
            threads.emplace_back([&stop] { while (!stop) Sleep(10); });
            if (i % 64 == 0) Log("OK: " + std::to_string(i) + " threads");
        }
    }
    catch (const std::exception& e)
    {
        Log(std::string("Falha ao criar thread: ") + e.what());
    }
    Log("Total de threads criadas: " + std::to_string(threads.size()));
    stop = true;
    for (auto& t : threads) t.join();
}

static void RunTests()
{
    init_apartment();
    auto folder = Windows::Storage::ApplicationData::Current().LocalFolder().Path();
    g_log.open(to_string(folder) + "\\resultado.txt", std::ios::trunc);

    Log("RPCS3-Xbox, Etapa 1");
    TestSystemInfo();
    TestJit();
    TestThreads();
    TestRam(); // por ultimo, pode derrubar o app
    Log("== FIM ==");
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
    IFrameworkView CreateView() { return *this; }
    void Initialize(CoreApplicationView const&) {}
    void Load(hstring const&) {}
    void Uninitialize() {}
    void SetWindow(CoreWindow const&) {}

    void Run()
    {
        auto window = CoreWindow::GetForCurrentThread();
        window.Activate();
        std::thread(RunTests).detach();
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);
    }
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    CoreApplication::Run(make<App>());
}
