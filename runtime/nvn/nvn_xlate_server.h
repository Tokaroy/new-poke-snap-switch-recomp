// A long-lived shader translator process (runtime/nvn/nvn_shader.cpp RunHarness). Header only, no Vulkan: tests/nvn/xlate_server_test.cpp includes it.
//
// RunHarness used to start `ShaderHarness --nvn ...` once per cache miss. Measured on 40 programs spread over the game's 1,639 (BelowNormal, this
// PC): 73.7 s for 40 processes (median 1.27 s each, nearly all of it process start and the translator's JIT warm-up) against 2.3 s for the same
// 40 through one `--nvn-serve` process (first request 1.6 s, median 15 ms), with byte-identical .spv and .json files. A first run translates
// hundreds of programs, so this is most of the first-visit stutter on an empty cache.
//
// Protocol (scripts/shader_harness/Program.cs NvnServe): one line per request on stdin, "data.bin TAB out.spv TAB out.json TAB stage TAB constOff
// TAB codeOff"; one line back, "ok" or "err TAB message". The process ends when stdin closes.
//
// One request at a time (callers queue behind a mutex: a request is milliseconds once the process is warm), BelowNormal priority, a job object so
// the child never outlives the game, a per-request time limit, and the process is closed after it has been idle for a while. Any trouble with the
// process (it cannot start, it is an older harness without the mode, it dies, it hangs) is reported as Unavailable and the caller translates that
// shader the old way, one process; after repeated trouble the server is not used again in this run.
#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nvn::xlate {

class Server {
 public:
  enum class Result { Ok, Failed, Unavailable };   // Failed: the translator rejected the shader (as a failing --nvn run did)

  explicit Server(std::string cmdline, DWORD requestMs = 10 * 60 * 1000, DWORD idleMs = 45 * 1000) : cmd_(std::move(cmdline)), requestMs_(requestMs), idleMs_(idleMs) {}
  ~Server() { Stop(); }
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  Result Translate(const std::string& bin, const std::string& spv, const std::string& json, int stage, uint32_t constOff, uint32_t codeOff, std::string* msg = nullptr) {
    std::lock_guard<std::mutex> lk(mu_);
    if (disabled_) return Result::Unavailable;
    if (!proc_ && !StartLocked()) return Fail();
    char head[96];
    snprintf(head, sizeof head, "\t%d\t%u\t%u\n", stage, constOff, codeOff);
    const std::string req = bin + "\t" + spv + "\t" + json + head;
    DWORD wrote = 0;
    if (!WriteFile(in_, req.data(), (DWORD)req.size(), &wrote, nullptr) || wrote != req.size()) { StopLocked(); return Fail(); }
    std::string line;
    const int rc = ReadLine(&line);
    if (rc != 1) { StopLocked(); return Fail(); }   // process gone or too slow
    served_++;
    lastUseMs_ = Now();
    wake_.notify_all();
    if (line == "ok") return Result::Ok;
    if (msg) *msg = line.size() > 4 ? line.substr(4) : line;
    return Result::Failed;
  }

  void Stop() {
    std::thread t;
    {
      std::lock_guard<std::mutex> lk(mu_);
      StopLocked();
      stopIdle_ = true;
      t = std::move(idleThread_);
    }
    wake_.notify_all();
    if (t.joinable()) t.join();
  }
  int starts() const { return starts_.load(); }
  bool disabled() const { std::lock_guard<std::mutex> lk(mu_); return disabled_; }
  bool running() const { std::lock_guard<std::mutex> lk(mu_); return proc_ != nullptr; }
  uint64_t served() const { std::lock_guard<std::mutex> lk(mu_); return served_; }

 private:
  static uint64_t Now() { return (uint64_t)GetTickCount64(); }

  Result Fail() {
    // a process that never answered a request is not a server (an older harness without --nvn-serve): do not try again; otherwise allow a few restarts
    failures_++;
    if (served_ == 0 || failures_ >= 5) disabled_ = true;
    return Result::Unavailable;
  }

  bool StartLocked() {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)) {
      if (inR) { CloseHandle(inR); CloseHandle(inW); }
      return false;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);   // the parent's ends stay out of the child
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = nul != INVALID_HANDLE_VALUE ? nul : outW;
    PROCESS_INFORMATION pi{};
    std::string cmd = cmd_;
    const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    CloseHandle(inR); CloseHandle(outW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(inW); CloseHandle(outR); return false; }
    if (!job_) {
      job_ = CreateJobObjectA(nullptr, nullptr);
      if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li);
      }
    }
    if (job_) AssignProcessToJobObject(job_, pi.hProcess);   // best effort: the game may already sit in a job without nesting rights
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess; in_ = inW; out_ = outR;
    pending_.clear();
    starts_++;
    lastUseMs_ = Now();
    if (!idleThread_.joinable() && !stopIdle_ && idleMs_ && idleMs_ != INFINITE) idleThread_ = std::thread([this] { IdleLoop(); });
    return true;
  }

  void StopLocked() {
    if (!proc_) return;
    CloseHandle(in_);   // EOF on stdin ends the process normally
    in_ = nullptr;
    if (WaitForSingleObject(proc_, 3000) != WAIT_OBJECT_0) TerminateProcess(proc_, 1);   // our own child only
    CloseHandle(proc_); CloseHandle(out_);
    proc_ = nullptr; out_ = nullptr;
  }

  // 1 = a line was read, 0 = the process ended or the time limit passed
  int ReadLine(std::string* line) {
    const uint64_t t0 = Now();
    for (int spin = 0;; spin++) {
      const size_t nl = pending_.find('\n');
      if (nl != std::string::npos) { *line = pending_.substr(0, nl); pending_.erase(0, nl + 1); if (!line->empty() && line->back() == '\r') line->pop_back(); return 1; }
      DWORD avail = 0;
      if (!PeekNamedPipe(out_, nullptr, 0, nullptr, &avail, nullptr)) return 0;   // broken pipe: the process is gone
      if (avail) {
        char buf[512]; DWORD got = 0;
        if (!ReadFile(out_, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || !got) return 0;
        pending_.append(buf, got);
        continue;
      }
      if (WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) {   // exited: take what it wrote last, then give up
        if (!PeekNamedPipe(out_, nullptr, 0, nullptr, &avail, nullptr) || !avail) return 0;
        continue;
      }
      if (requestMs_ != INFINITE && Now() - t0 > requestMs_) return 0;
      if (spin > 200) Sleep(1); else Sleep(0);
    }
  }

  void IdleLoop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stopIdle_) {
      wake_.wait_for(lk, std::chrono::milliseconds(idleMs_ / 3 + 1));
      if (stopIdle_) break;
      if (proc_ && Now() - lastUseMs_ >= idleMs_) StopLocked();   // idle: give the memory back; the next miss starts a new one
    }
  }

  const std::string cmd_;
  const DWORD requestMs_, idleMs_;
  mutable std::mutex mu_;
  std::condition_variable wake_;
  std::thread idleThread_;
  bool stopIdle_ = false, disabled_ = false;
  HANDLE proc_ = nullptr, in_ = nullptr, out_ = nullptr, job_ = nullptr;
  std::string pending_;
  uint64_t served_ = 0, lastUseMs_ = 0;
  int failures_ = 0;
  std::atomic<int> starts_{0};
};

// Up to `max` Servers for one command line, for callers on several threads (the background shader build, nvn_shader.cpp ProgramJob): a caller
// borrows an idle process, starts a new one while under the limit, or waits until one is handed back. The most recently returned process is
// lent first, so a short burst keeps reusing warm processes. Never shrinks; each Server still closes its own process when idle.
class Pool {
 public:
  Pool(std::string cmdline, int max) : cmd_(std::move(cmdline)), max_(std::max(1, max)) {}
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  Server* Acquire() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
      if (!idle_.empty()) { Server* s = idle_.back(); idle_.pop_back(); return s; }
      if ((int)all_.size() < max_) { all_.push_back(std::make_unique<Server>(cmd_)); return all_.back().get(); }
      cv_.wait(lk);
    }
  }
  void Release(Server* s) {
    { std::lock_guard<std::mutex> lk(mu_); idle_.push_back(s); }
    cv_.notify_one();
  }
  // Translate on a borrowed process (Server::Translate's results).
  Server::Result Translate(const std::string& bin, const std::string& spv, const std::string& json, int stage, uint32_t constOff, uint32_t codeOff, std::string* msg = nullptr) {
    Server* s = Acquire();
    const Server::Result r = s->Translate(bin, spv, json, stage, constOff, codeOff, msg);
    Release(s);
    return r;
  }
  int servers() const { std::lock_guard<std::mutex> lk(mu_); return (int)all_.size(); }
  int starts() const {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (auto& s : all_) n += s->starts();
    return n;
  }

 private:
  const std::string cmd_;
  const int max_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::unique_ptr<Server>> all_;
  std::vector<Server*> idle_;
};

}  // namespace nvn::xlate
