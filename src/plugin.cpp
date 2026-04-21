// MW Drift Controller by AndyQinke - ASI for Need for Speed Most Wanted (2005)
// 内存地址按“首选映像基址”换算：实际指针 = VA - ImageBase + GetModuleHandle(nullptr)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>

namespace {

constexpr uintptr_t kDefaultImageBase = 0x00400000;

uintptr_t g_imageBase = kDefaultImageBase;
HMODULE g_selfModule = nullptr;

uintptr_t GameBase() {
  return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

uintptr_t VaToPtr(uintptr_t absVa) {
  if (absVa < g_imageBase) return absVa + GameBase();
  return absVa - g_imageBase + GameBase();
}

float* F(uintptr_t absVa) {
  return reinterpret_cast<float*>(VaToPtr(absVa));
}

// 默认绝对 VA（你提供的地址）；可在 INI「内存地址_*」中覆盖（支持 0x 前缀十六进制）
uintptr_t g_vaSpeed = 0x00914654;
uintptr_t g_vaFrontSteer = 0x0089095C;
uintptr_t g_vaMinDriftBase = 0x008ABB7C;
uintptr_t g_vaMinSlipRad = 0x008ABB78;
uintptr_t g_vaFrictionScale = 0x00891050;
uintptr_t g_vaMaxSteerAngle = 0x008AADE8;
uintptr_t g_vaSteeringScale = 0x008AB22C;
uintptr_t g_vaBodyYaw = 0x009386D0;

std::unordered_map<std::string, std::string> g_ini;
std::wstring g_iniPath;

std::string Trim(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
  return s;
}

bool LoadIniUtf8(const std::wstring& path) {
  g_ini.clear();
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
    data.erase(data.begin(), data.begin() + 3);
  }
  std::string line;
  for (size_t i = 0; i < data.size(); ++i) {
    const char c = data[i];
    if (c == '\n') {
      line = Trim(line);
      if (!line.empty() && line[0] != '#' && line[0] != ';') {
        const auto eq = line.find('=');
        if (eq != std::string::npos) {
          std::string k = Trim(line.substr(0, eq));
          std::string v = Trim(line.substr(eq + 1));
          if (!k.empty()) g_ini[k] = v;
        }
      }
      line.clear();
    } else if (c != '\r') {
      line.push_back(c);
    }
  }
  if (!line.empty()) {
    line = Trim(line);
    if (!line.empty() && line[0] != '#' && line[0] != ';') {
      const auto eq = line.find('=');
      if (eq != std::string::npos) {
        std::string k = Trim(line.substr(0, eq));
        std::string v = Trim(line.substr(eq + 1));
        if (!k.empty()) g_ini[k] = v;
      }
    }
  }
  return true;
}

const std::string* IniGet(const std::string& key) {
  auto it = g_ini.find(key);
  if (it == g_ini.end()) return nullptr;
  return &it->second;
}

float IniFloat(const std::string& key, float def) {
  const std::string* s = IniGet(key);
  if (!s || s->empty()) return def;
  try {
    return std::stof(*s);
  } catch (...) {
    return def;
  }
}

int IniInt(const std::string& key, int def) {
  const std::string* s = IniGet(key);
  if (!s || s->empty()) return def;
  try {
    return std::stoi(*s);
  } catch (...) {
    return def;
  }
}

bool IniBool(const std::string& key, bool def) {
  const std::string* s = IniGet(key);
  if (!s || s->empty()) return def;
  if (*s == '1' || *s == 'y' || *s == 'Y' || *s == 't' || *s == 'T') return true;
  if (*s == '0' || *s == 'n' || *s == 'N' || *s == 'f' || *s == 'F') return false;
  return def;
}

uintptr_t IniAddrVa(const std::string& key, uintptr_t defVa) {
  const std::string* s = IniGet(key);
  if (!s || s->empty()) return defVa;
  try {
    return static_cast<uintptr_t>(std::stoull(*s, nullptr, 0));
  } catch (...) {
    return defVa;
  }
}

void ReloadMemoryAddressesFromIni() {
  g_vaSpeed = IniAddrVa(std::string(u8"内存地址_当前车速"), g_vaSpeed);
  g_vaFrontSteer = IniAddrVa(std::string(u8"内存地址_前轮转向角度"), g_vaFrontSteer);
  g_vaMinDriftBase = IniAddrVa(std::string(u8"内存地址_漂移基础值"), g_vaMinDriftBase);
  g_vaMinSlipRad = IniAddrVa(std::string(u8"内存地址_滑动半径"), g_vaMinSlipRad);
  g_vaFrictionScale = IniAddrVa(std::string(u8"内存地址_摩擦力缩放"), g_vaFrictionScale);
  g_vaMaxSteerAngle = IniAddrVa(std::string(u8"内存地址_最大转向角度"), g_vaMaxSteerAngle);
  g_vaSteeringScale = IniAddrVa(std::string(u8"内存地址_转向缩放"), g_vaSteeringScale);
  g_vaBodyYaw = IniAddrVa(std::string(u8"内存地址_车身Y轴旋转"), g_vaBodyYaw);
}

std::wstring IniPathNextToDll() {
  wchar_t buf[MAX_PATH]{};
  GetModuleFileNameW(g_selfModule ? g_selfModule : GetModuleHandleW(nullptr), buf, MAX_PATH);
  std::wstring p(buf);
  const auto dot = p.find_last_of(L'.');
  if (dot == std::wstring::npos) return p + L".ini";
  return p.substr(0, dot) + L".ini";
}

bool KeyDown(int vk) {
  return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool KeyPressedEdge(int vk) {
  return (GetAsyncKeyState(vk) & 1) != 0;
}

double QpcSeconds() {
  static LARGE_INTEGER freq{};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER c{};
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

std::string Utf8OrbitalKey(int tier) {
  return std::string(u8"轨道转向角度配置") + std::to_string(tier);
}

std::string MaxSteerIniKey(int idx) {
  if (idx < 0) idx = 0;
  if (idx > 31) idx = 31;
  if (idx == 0) return std::string(u8"最大转向角度配置1");
  if (idx <= 9) return std::string(u8"最大转向角度配置1.") + std::to_string(idx);
  if (idx == 10) return std::string(u8"最大转向角度配置2");
  if (idx <= 19) return std::string(u8"最大转向角度配置2.") + std::to_string(idx - 10);
  if (idx == 20) return std::string(u8"最大转向角度配置3");
  if (idx <= 29) return std::string(u8"最大转向角度配置3.") + std::to_string(idx - 20);
  if (idx == 30) return std::string(u8"最大转向角度配置4");
  return std::string(u8"最大转向角度配置4.1");
}

float ReadMaxSteer(int idx, float defV) {
  return IniFloat(MaxSteerIniKey(idx), defV);
}

float ReadOrbital(int tier, float defV) {
  return IniFloat(Utf8OrbitalKey(tier), defV);
}

float Kmh(float rawGame) {
  return rawGame * IniFloat(std::string(u8"车速显示换算"), 3.6f);
}

void WritePhysics(float driftBase, float slipRad, float friction, float maxSteer, float steerScale, float yaw) {
  *F(g_vaMinDriftBase) = driftBase;
  *F(g_vaMinSlipRad) = slipRad;
  *F(g_vaFrictionScale) = friction;
  *F(g_vaMaxSteerAngle) = maxSteer;
  *F(g_vaSteeringScale) = steerScale;
  *F(g_vaBodyYaw) = yaw;
}

// 仅功能2会改写的量；从功能1首次进入功能2时快照，退出功能2回到功能1时写回
struct PhysicsSnap {
  float minDrift = 0, slip = 0, friction = 0, maxSteer = 0, steerScale = 0, yaw = 0;
  bool valid = false;
};

void CapturePreF2Physics(PhysicsSnap& snap) {
  snap.minDrift = *F(g_vaMinDriftBase);
  snap.slip = *F(g_vaMinSlipRad);
  snap.friction = *F(g_vaFrictionScale);
  snap.maxSteer = *F(g_vaMaxSteerAngle);
  snap.steerScale = *F(g_vaSteeringScale);
  snap.yaw = *F(g_vaBodyYaw);
  snap.valid = true;
}

void RestorePreF2Physics(PhysicsSnap& snap) {
  if (!IniBool(std::string(u8"功能2退出恢复物理"), true)) {
    snap.valid = false;
    return;
  }
  if (!snap.valid) return;
  WritePhysics(snap.minDrift, snap.slip, snap.friction, snap.maxSteer, snap.steerScale, snap.yaw);
  snap.valid = false;
}

enum class MainMode { F1Steer, F2Drift };

enum class Act { None = 0, A1 = 1, A2 = 2, A3 = 3, A4 = 4 };

enum class DPhase { WaitInput, Op1, Op2, Op3 };

struct Drift {
  bool on = false;
  Act act = Act::None;
  DPhase phase = DPhase::WaitInput;
  double tEnter = 0;
  double tOp3 = 0;
  double ladder0 = 0;
  double tOp2 = 0;
  bool oppPrev = false;
  bool samePrev = false;
  bool noLRArmed = false;
  double noLRSince = 0;
  double op2ExitSince = 0;

  double a1 = 0;
  bool a1Arm = false;
  double a2 = 0;
  bool a2Arm = false;
  double a3 = 0;
  double a4 = 0;
};

struct SteerAssist {
  double hold = 0;
  int tier = 1;
  int tierFloor = 1;
  bool lerpOn = false;
  double lerpT0 = 0;
  float lerpA = 0;
  float lerpB = 0;
  float lastSign = 1.f;
  double dl = 0;
  double dr = 0;
  bool prevLR = false;
};

std::string TierVmaxKmHKey(int tier) {
  return std::string(u8"功能1轨道档位") + std::to_string(tier) + u8"最高车速_KmH";
}

int TierFromHold(double hold, float kmh) {
  static const float kDefVmax15_31[17] = {350, 340, 330, 320, 310, 300, 290, 280, 270, 260,
                                          250, 240, 230, 220, 210, 200, 190};
  const float low = IniFloat(std::string(u8"功能1最低车速_KmH"), 50.f);
  const float hardHigh = IniFloat(std::string(u8"功能1最高车速_KmH"), 600.f);
  const float vmaxLowTiers = IniFloat(std::string(u8"功能1档位1至14最高车速_KmH"), 600.f);
  if (kmh <= low || kmh >= hardHigh) return 1;
  int best = 1;
  for (int tier = 1; tier <= 31; ++tier) {
    const double lo = (tier <= 1) ? 0.0 : 0.1 * static_cast<double>(tier - 1);
    const float vmax =
        (tier <= 14) ? vmaxLowTiers
                     : IniFloat(TierVmaxKmHKey(tier), kDefVmax15_31[static_cast<size_t>(tier - 15)]);
    if (hold > lo && kmh < vmax) best = std::max(best, tier);
  }
  return std::min(31, std::max(1, best));
}

void DriftApplyPhysics(Act act, DPhase ph, int maxIdx, float maxSteerVal) {
  const float db = IniFloat(std::string(u8"漂移基础值"), 1.f);
  const float slipMin = IniFloat(std::string(u8"最小滑动半径"), 1.f);
  const float slipMax = IniFloat(std::string(u8"最大滑动半径"), 1.f);
  const float fricMin = IniFloat(std::string(u8"最小摩擦力缩放"), 1.f);
  const float fricMax = IniFloat(std::string(u8"最大摩擦力缩放"), 1.f);
  const float scMin = IniFloat(std::string(u8"最小转向缩放"), 1.f);
  const float scMax = IniFloat(std::string(u8"最大转向缩放"), 1.f);
  const float yLLm = IniFloat(std::string(u8"向左最小旋转速度"), 0.f);
  const float yLLM = IniFloat(std::string(u8"向左最大旋转速度"), 0.f);
  const float yRRm = IniFloat(std::string(u8"向右最小旋转速度"), 0.f);
  const float yRRM = IniFloat(std::string(u8"向右最大旋转速度"), 0.f);

  float slip = slipMin, yaw = 0.f;
  float sc = scMin;

  const bool op1 = (ph == DPhase::Op1 || ph == DPhase::WaitInput);
  const bool op2 = (ph == DPhase::Op2);
  const bool op3 = (ph == DPhase::Op3);

  switch (act) {
    case Act::A1:
    case Act::A3:
      if (op1) {
        slip = slipMin;
        sc = scMin;
        yaw = yLLm;
      } else if (op2) {
        slip = slipMax;
        sc = scMax;
        yaw = yRRM;
      } else if (op3) {
        slip = slipMin;
        sc = scMin;
        yaw = yLLM;
      }
      break;
    case Act::A2:
    case Act::A4:
      if (op1) {
        slip = slipMin;
        sc = scMin;
        yaw = yRRm;
      } else if (op2) {
        slip = slipMax;
        sc = scMax;
        yaw = yLLM;
      } else if (op3) {
        slip = slipMin;
        sc = scMin;
        yaw = yRRM;
      }
      break;
    default:
      break;
  }
  (void)maxIdx;
  (void)ph;
  (void)fricMin;
  WritePhysics(db, slip, fricMax, maxSteerVal, sc, yaw);
}

DWORD WINAPI ThreadMain(LPVOID) {
  constexpr double hz = 120.0;
  const double dt = 1.0 / hz;

  MainMode mode = MainMode::F1Steer;
  Drift d{};
  SteerAssist s{};
  PhysicsSnap preF2{};

  while (true) {
    const double now = QpcSeconds();
    static double lastIni = 0;
    if (now - lastIni > 0.5) {
      LoadIniUtf8(g_iniPath);
      ReloadMemoryAddressesFromIni();
      g_imageBase = static_cast<uintptr_t>(IniInt("ImageBase", static_cast<int>(kDefaultImageBase)));
      lastIni = now;
    }

    const float f2EnterKmh = IniFloat(std::string(u8"功能2激活最低车速_KmH"), 60.f);
    const float f1BandLow = IniFloat(std::string(u8"功能1最低车速_KmH"), 50.f);
    const float f1BandHigh = IniFloat(std::string(u8"功能1最高车速_KmH"), 600.f);
    const float noLRSec = IniFloat(std::string(u8"功能2无方向键退出秒"), 2.f);
    const float op3Sec = IniFloat(std::string(u8"功能2操作模式3持续秒"), 3.f);
    const float op2ExitDelay = IniFloat(std::string(u8"功能2模式2配置3反向键退出延迟秒"), 0.1f);
    const float op1LadderStep = std::max(0.01f, IniFloat(std::string(u8"功能2操作模式1阶梯间隔秒"), 0.1f));
    const float op2ReverseSec = std::max(0.01f, IniFloat(std::string(u8"功能2操作模式2倒序总秒"), 1.f));
    constexpr int kOp2RevSteps = 11;
    const double op2StepDur = op2ReverseSec / static_cast<double>(kOp2RevSteps);
    const float comboCfg5Sec = IniFloat(std::string(u8"下键与方向轨道转向角度配置5秒"), 1.5f);
    const float orbitLerpSec = std::max(0.01f, IniFloat(std::string(u8"轨道转向角度回退秒"), 3.f));
    const float steerAngMin = IniFloat(std::string(u8"最小转向角度"), 0.5f);
    const float steerAngMax = IniFloat(std::string(u8"最大转向角度"), 1.f);

    const float kmh = Kmh(*F(g_vaSpeed));
    const bool L = KeyDown(VK_LEFT);
    const bool R = KeyDown(VK_RIGHT);
    const bool D = KeyDown(VK_DOWN);
    const bool Sp = KeyDown(VK_SPACE);

    const float ii = std::max(0.01f, IniFloat(std::string(u8"功能2激活按键输入秒"), 0.35f));

    // ---- 功能2：激活（任意时刻满足都可覆盖当前漂移状态）----
    if (kmh >= f2EnterKmh) {
      Act trig = Act::None;

      if (L && !Sp) d.a1 += dt;
      else {
        d.a1 = 0;
        d.a1Arm = false;
      }
      if (L && d.a1 >= ii) d.a1Arm = true;
      if (d.a1Arm && KeyPressedEdge(VK_SPACE) && L) trig = Act::A1;

      if (R && !Sp) d.a2 += dt;
      else {
        d.a2 = 0;
        d.a2Arm = false;
      }
      if (R && d.a2 >= ii) d.a2Arm = true;
      if (d.a2Arm && KeyPressedEdge(VK_SPACE) && R) trig = Act::A2;

      if (L && Sp) d.a3 += dt;
      else d.a3 = 0;
      if (R && Sp) d.a4 += dt;
      else d.a4 = 0;

      if (L && Sp && d.a3 >= ii) {
        trig = Act::A3;
        d.a3 = 0;
      }
      if (R && Sp && d.a4 >= ii) {
        trig = Act::A4;
        d.a4 = 0;
      }

      if (trig != Act::None) {
        if (mode == MainMode::F1Steer && IniBool(std::string(u8"功能2退出恢复物理"), true)) CapturePreF2Physics(preF2);
        else if (mode == MainMode::F1Steer) preF2.valid = false;
        d = Drift{};
        d.on = true;
        d.act = trig;
        d.phase = DPhase::WaitInput;
        d.tEnter = now;
        d.ladder0 = now;
        d.oppPrev = (trig == Act::A1 || trig == Act::A3) ? R : L;
        d.samePrev = (trig == Act::A1 || trig == Act::A3) ? L : R;
        d.noLRArmed = false;
        d.op2ExitSince = 0;
        mode = MainMode::F2Drift;
      }
    }

    if (mode == MainMode::F2Drift && kmh < f2EnterKmh) {
      RestorePreF2Physics(preF2);
      mode = MainMode::F1Steer;
      d = Drift{};
    }

    if (mode == MainMode::F2Drift && d.on) {
      const bool opp = (d.act == Act::A1 || d.act == Act::A3) ? R : L;
      const bool same = (d.act == Act::A1 || d.act == Act::A3) ? L : R;

      if (!L && !R) {
        if (!d.noLRArmed) {
          d.noLRArmed = true;
          d.noLRSince = now;
        }
        if (now - d.noLRSince >= static_cast<double>(noLRSec)) {
          RestorePreF2Physics(preF2);
          mode = MainMode::F1Steer;
          d = Drift{};
          s = SteerAssist{};
        }
      } else {
        if (d.noLRArmed && (now - d.noLRSince < static_cast<double>(noLRSec))) {
          d.phase = DPhase::WaitInput;
          d.oppPrev = (d.act == Act::A1 || d.act == Act::A3) ? R : L;
          d.samePrev = (d.act == Act::A1 || d.act == Act::A3) ? L : R;
          d.op2ExitSince = 0.0;
        }
        d.noLRArmed = false;
      }

      if (mode == MainMode::F2Drift && d.on) {
        const bool sameEdge = same && !d.samePrev;
        d.samePrev = same;
        const bool oppEdge = opp && !d.oppPrev;
        d.oppPrev = opp;

        if (sameEdge && d.phase != DPhase::Op3) {
          d.phase = DPhase::Op3;
          d.tOp3 = now;
        }

        if (d.phase == DPhase::Op3) {
          // “最大转向角度配置2”对应阶梯中 2.0（整数键 配置2）即 idx=10
          const float ms = ReadMaxSteer(10, std::max(steerAngMin, std::min(steerAngMax, *F(g_vaMaxSteerAngle))));
          DriftApplyPhysics(d.act, DPhase::Op3, 10, ms);
          if (now - d.tOp3 >= static_cast<double>(op3Sec)) {
            RestorePreF2Physics(preF2);
            mode = MainMode::F1Steer;
            d = Drift{};
            s = SteerAssist{};
          }
        } else if (d.phase == DPhase::Op2) {
          const double u = now - d.tOp2;
          const int step = std::min(kOp2RevSteps - 1, static_cast<int>(std::floor(u / op2StepDur)));
          static const int rev[11] = {30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20};
          const int idx = rev[step];
          const float ms = ReadMaxSteer(idx, std::max(steerAngMin, std::min(steerAngMax, *F(g_vaMaxSteerAngle))));
          DriftApplyPhysics(d.act, DPhase::Op2, idx, ms);

          if (idx == 20 && opp) {
            if (d.op2ExitSince == 0.0) d.op2ExitSince = now;
            if (now - d.op2ExitSince >= static_cast<double>(op2ExitDelay)) {
              RestorePreF2Physics(preF2);
              mode = MainMode::F1Steer;
              d = Drift{};
              s = SteerAssist{};
            }
          } else {
            d.op2ExitSince = 0.0;
          }

          if (step >= kOp2RevSteps - 1 && !opp) {
            d.phase = DPhase::WaitInput;
          }
        } else {
          if (opp) {
            if (oppEdge) d.ladder0 = now;
            d.phase = DPhase::Op1;
            int idx = static_cast<int>(std::floor((now - d.ladder0) / static_cast<double>(op1LadderStep)));
            idx = std::max(0, std::min(31, idx));
            const float ms = ReadMaxSteer(idx, std::max(steerAngMin, std::min(steerAngMax, *F(g_vaMaxSteerAngle))));
            DriftApplyPhysics(d.act, DPhase::Op1, idx, ms);
            if (idx >= 31) {
              d.phase = DPhase::Op2;
              d.tOp2 = now;
              d.op2ExitSince = 0.0;
            }
          } else {
            if (d.phase == DPhase::Op1) d.phase = DPhase::WaitInput;
            const float ms = ReadMaxSteer(0, std::max(steerAngMin, std::min(steerAngMax, *F(g_vaMaxSteerAngle))));
            DriftApplyPhysics(d.act, DPhase::WaitInput, 0, ms);
          }
        }
      }
    } else {
      // ---- 功能1 ----
      const bool inBand = (kmh > f1BandLow && kmh < f1BandHigh);
      const bool LR = L || R;

      if (LR && inBand) {
        if (s.lerpOn) {
          s.lerpOn = false;
          s.hold = 0;
        }
        s.hold += dt;
        const int tHold = TierFromHold(s.hold, kmh);
        s.tier = std::max(s.tierFloor, tHold);

        if (L && D) s.dl += dt;
        else s.dl = 0;
        if (R && D) s.dr += dt;
        else s.dr = 0;
        const bool combo5 = (s.dl >= static_cast<double>(comboCfg5Sec) || s.dr >= static_cast<double>(comboCfg5Sec));

        float mag = ReadOrbital(combo5 ? 5 : s.tier, 0.f);
        const float sgn = (L && !R) ? -1.f : 1.f;
        s.lastSign = sgn;
        *F(g_vaFrontSteer) = mag * sgn;
      } else {
        if (s.prevLR && !LR) s.tierFloor = s.tier;
        s.hold = 0;

        if (inBand) {
          if (!s.lerpOn) {
            s.lerpOn = true;
            s.lerpT0 = now;
            s.lerpA = *F(g_vaFrontSteer);
            const float c1 = ReadOrbital(1, 0.f);
            s.lerpB = c1 * (s.lastSign >= 0 ? 1.f : -1.f);
          }
          const double u = std::min(1.0, (now - s.lerpT0) / static_cast<double>(orbitLerpSec));
          *F(g_vaFrontSteer) = static_cast<float>(s.lerpA + (s.lerpB - s.lerpA) * u);
          if (u >= 1.0 - 1e-9) {
            s.tier = 1;
            s.tierFloor = 1;
            s.lerpOn = false;
          }
        }

        if (LR && s.lerpOn) {
          s.lerpOn = false;
          s.hold = 0;
        }
      }
      s.prevLR = LR;
    }

    Sleep(static_cast<DWORD>(dt * 1000.0));
  }
  return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    g_selfModule = hModule;
    g_iniPath = IniPathNextToDll();
    LoadIniUtf8(g_iniPath);
    ReloadMemoryAddressesFromIni();
    g_imageBase = static_cast<uintptr_t>(IniInt("ImageBase", static_cast<int>(kDefaultImageBase)));
    CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr);
  }
  return TRUE;
}
