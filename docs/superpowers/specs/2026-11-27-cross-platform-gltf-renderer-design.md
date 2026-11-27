# Cross-Platform glTF Renderer — Tasarım Dokümanı

- Tarih: 2026-11-27
- Durum: TASLAK (kullanıcı onayı bekliyor)
- Uygulayıcı model: Opus 5.5 · Kontrol/review modeli: Fable 5.2

---

## 1. Amaç ve neden

`.glb` / `.gltf` modellerini glTF 2.0 standardının **tamamıyla** (core + Khronos ratified extension'lar + seçili EXT extension'lar) render eden, **native grafik API'leri** üzerinde çalışan, **platform bağımsız** bir renderer.

Kullanıcının öncelikleri:
1. **Native**: her platformda o platformun kendi API'si; çeviri katmanı (wgpu/Dawn native, MoltenVK, ANGLE) yok.
2. **Tam glTF kapsamı**: standarttaki her özellik doğru render edilir.
3. **Aç/kapat sistemi**: ek glTF özellikleri ve render özellikleri; (a) API/donanım desteğine göre otomatik, (b) Unreal benzeri settings sistemiyle elle açılıp kapanır.
4. **Upscaler'lar**: DLSS, FSR, XeSS (ve Apple'da MetalFX) desteklendiği her yerde native entegrasyon.

Bu bir **renderer**dır, oyun motoru değil (fizik, ses, oyun mantığı, editör kapsam dışı — bkz. §2.2).

## 2. Hedefler

### 2.1 Hedefler
- Platformlar: Windows, Linux, macOS, iOS, Android, Web (tarayıcı).
- Backend'ler (hepsi native, kendi kodumuz):

| Backend | Platformlar | Not |
|---|---|---|
| D3D12 | Windows | birincil Windows backend |
| D3D11 | Windows | Feature Level 11_0 / 11_1 **ve** 10_0 / 10_1 ("DX10" ihtiyacı burada karşılanır — ayrı D3D10 backend YAZILMAZ, bkz. §4) |
| Vulkan 1.2+ (1.3 tercih) | Windows, Linux, Android | Android birincil |
| Metal 3 | macOS, iOS | Mac yok → bkz. §3 kısıt K2 |
| OpenGL ES 3.0 / 3.2 | Android (eski cihaz yedek) | yedek backend |
| WebGPU | Web (Emscripten + `webgpu.h`, emdawnwebgpu) | web birincil; tarayıcının kendi WebGPU'sunu doğrudan çağırır |
| WebGL2 | Web yedek | GLES3 backend'i ile kodun çoğunu paylaşır |

- glTF 2.0 tam kapsam (bkz. §7) + Khronos glTF-Validator temiz yükleme.
- Kapasite çözümleme sistemi (asset × backend caps × setting) → shader permutation (bkz. §5, §6).
- Unreal tarzı settings: CVar'lar (`r.*`), scalability grupları (`sg.*`), preset'ler, ini katmanları, runtime değişim (bkz. §8).
- Upscaler entegrasyonu (bkz. §9).
- Headless, otomatik doğrulanabilir render (golden-image testleri) — her fazın kalite kapısı (bkz. §10).

### 2.2 Non-goals (kapsam dışı)
- Oyun motoru alt sistemleri: fizik, ses, input-tabanlı oyun mantığı, scripting, networking.
- Editör / level editor. (Sadece bir **viewer** örnek uygulaması + debug UI: Dear ImGui.)
- glTF **export**/yazma.
- Konsol platformları (Xbox/PlayStation/Switch) — NDA SDK'ları gerektirir.
- Ayrı bir D3D10 API backend'i (D3D11 FL10 ile karşılanır).

### 2.3 Anti-goals (teknik olarak çalışsa bile başarısızlık sayılır)
- Bir backend'i başka bir API'ye çeviri katmanıyla "desteklemek" (ör. Metal yerine MoltenVK, Web'de WebGPU yerine wgpu-native üstünden ikinci katman).
- Bir glTF özelliğinin bir backend'de **sessizce** yanlış render edilmesi. Desteklenmeyen özellik ya belgelenmiş bir fallback'e düşer ya da log + debug UI'da "OFF (neden)" görünür.
- Doğrulanmamış "tamamlandı" iddiası: her faz reviewer'ın **kendi çalıştırdığı** build + render + image-diff ile kapanır.
- Mac'te hiç çalıştırılmamış Metal kodunu "çalışıyor" diye raporlamak.

## 3. Kısıtlar (bu makinede doğrulandı, 2026-11-27)

| # | Kısıt | Etki |
|---|---|---|
| K1 | Geliştirme makinesi: Windows 11 Pro, **NVIDIA RTX 3070 Ti** (driver 32.0.16.2140), VS 2026 Enterprise + BuildTools, Windows SDK 10.0.22621/26100/28000, CMake, Ninja, Git, Python 3.14, Vulkan SDK 1.4.341.1 (DXC dahil) | D3D11, D3D12, Vulkan bu makinede **derlenip çalıştırılabilir**. DLSS çalıştırılabilir (RTX). FSR analitik + XeSS-SR (SM6.4 DP4a) çalıştırılabilir. FSR 4 ML (AMD-only) **çalıştırılamaz**. |
| K2 | **Mac / iOS cihaz yok** | Metal + iOS backend'i: **sadece derleme + shader doğrulama** (GitHub Actions macOS runner: `xcrun metal` ile MSL derleme, Xcode build). Hosted macOS runner'da GPU erişimi garanti DEĞİL → runtime render testi **doğrulanmamış** sayılır. Her faz raporunda bu tekrar yazılır. İleride gerçek Mac (kiralık: AWS EC2 Mac / MacinCloud) ile doğrulama ayrı bir faz. |
| K3 | Emscripten yok, Android SDK/NDK yok | İlgili fazın başında kurulur (emsdk; Android Studio cmdline-tools + NDK + emulator). Android runtime testi **emülatörde** (Vulkan: host-GPU gfxstream; GLES). Fiziksel cihaz yok → gerçek cihaz davranışı doğrulanmamış sayılır. |
| K4 | Klasör git repo değil | Faz 0'da `git init` (kullanıcı onayı ile). |
| K5 | DLSS/XeSS SDK'ları kapalı kaynak, lisanslı | SDK'lar repoya **commit edilmez**; `third_party/` altına kullanıcının indirdiği yerden CMake option ile bağlanır. Yoksa özellik derleme dışı kalır (capability = OFF, neden: "SDK yok"). |

## 4. Yaklaşımlar (değerlendirilen ve seçilen)

| Yaklaşım | Artı | Eksi | Karar |
|---|---|---|---|
| **A. Tüm RHI'yi kendimiz yazmak** (her backend native, kendi kodumuz) | Tam kontrol; "native" önceliğiyle birebir; upscaler native handle erişimi en temiz | En büyük iş yükü; 6+ backend'in senkronizasyon/bellek/shader hattı | **SEÇİLDİ** (kullanıcı: "native öncelikli") |
| B. Diligent Engine'i RHI olarak kullanmak | En hızlı ilk görüntü; tüm backend'ler hazır | Dış bağımlılık; alt seviye kontrol az | Reddedildi (native-sahiplik önceliği) |
| C. WebGPU'yu ortak katman yapmak (Dawn/wgpu native) | Tek API | Native değil (DX12/VK/Metal üstünde çeviri); D3D11 yok; DLSS/XeSS native erişimi zayıf | Reddedildi |
| D3D10 ayrı backend | "DX10" listesi birebir | D3D10 API'si ölü; D3D11 FL10_0/10_1 aynı donanımı kapsar | Reddedildi → D3D11 FL10 |

**Web notu (kullanıcı sorusu):** wgpu tarayıcıda kendisi render etmez; wasm'a derlenip tarayıcının WebGPU'sunu çağırır (Chrome/Edge=Dawn, Firefox=wgpu, Safari=WebKit). C++ + Emscripten + `webgpu.h` aynı API'yi doğrudan çağırır → aynı performans, ekstra katman yok.

## 5. Mimari

### 5.1 Dil / build
- C++20, CMake (≥3.28) + Ninja; MSVC (Windows), Clang (Android NDK, Apple, Emscripten, Linux).
- CMake preset'leri: `win-d3d12`, `win-d3d11`, `win-vulkan`, `linux-vulkan`, `android-vulkan`, `android-gles`, `macos-metal`, `ios-metal`, `web-webgpu`, `web-webgl2`, ayrıca `win-all` (tüm Windows backend'leri tek binary'de, runtime seçim).
- Bağımlılık yönetimi: CMake `FetchContent` (sürüm pinli). (Builder'a bırakıldı: vcpkg'ye geçiş.)

### 5.2 Modüller (dizin yapısı)
```
engine/
  core/        # log, assert, bellek, job system, math (kendi veya glm — builder kararı), cvar sistemi
  platform/    # pencere/surface: Win32, X11/Wayland, Cocoa/UIKit, Android NativeActivity, Emscripten canvas
  rhi/         # backend-bağımsız arayüz (Device, Queue, CommandList, Buffer, Texture, Sampler,
               #  PipelineState, BindGroup/DescriptorSet, Fence, Swapchain, QueryPool) + DeviceCaps
    d3d12/ d3d11/ vulkan/ metal/ gles/ webgpu/   # gles/ hem GLES3 hem WebGL2'yi kapsar
  shader/      # offline shader derleyici aracı + runtime permutation cache
  asset/       # glTF yükleyici (fastgltf), decoder'lar (Draco, meshoptimizer, Basis/KTX2, WebP/AVIF), GPU upload
  scene/       # node hiyerarşisi, transform, kamera, ışık, skin, morph, animasyon, variants
  renderer/    # frame graph, pass'ler (depth prepass, shadow, opaque, transmission, transparent,
               #  post: tonemap/bloom/TAA), IBL, capability resolver
  upscale/     # IUpscaler arayüzü + dlss/ fsr/ xess/ metalfx/ (her biri koşullu derleme)
  settings/    # scalability grupları, preset'ler, ini katmanları
apps/
  viewer/      # örnek glTF viewer + ImGui debug UI (capability tablosu, cvar konsolu)
  headless/    # offscreen render → PNG (test kapısı)
tools/
  shaderc/     # HLSL → DXIL / SPIR-V / MSL / WGSL / GLSL ES
  golden/      # image-diff (FLIP veya PSNR/SSIM) + rapor
tests/
  unit/  golden/  assets/ (glTF-Sample-Assets, submodule/fetch)
```

### 5.3 RHI tasarım ilkeleri
- **Explicit API modeli** (D3D12/Vulkan/Metal benzeri): command list, pipeline state object, explicit barrier/state geçişi. D3D11, GLES ve WebGPU backend'leri bu modeli içeride emüle eder (barrier → no-op ya da implicit).
- Bind modeli: "bind group" soyutlaması (WebGPU'nun en kısıtlı modeli taban alınır), üst tier'larda bindless opsiyonel extension.
- **Overfit önleme:** RHI arayüzü **ilk günden iki farklı karakterde backend'e** karşı yazılır: Vulkan (explicit) + D3D11 (implicit). D3D12 üçüncü gelir.
- `DeviceCaps` yapısı: her backend kendi gerçek kapasitesini sorgulayıp doldurur (texture formatları, BC/ASTC/ETC2 desteği, max bind, compute, storage buffer, MSAA, float16, wave ops, ray tracing, bindless, vb.).
- Native handle erişimi: `IDevice::getNativeHandle()` — upscaler'lar ve ileride üçüncü parti entegrasyonlar için.

### 5.4 Shader hattı (**ADLANDIRILMIŞ RİSK — Faz 1 spike**)
- Tek kaynak dil: **HLSL** (SM 6.x).
- Derleme:
  - D3D12: DXC → DXIL
  - D3D11: DXC → SPIR-V → SPIRV-Cross → HLSL SM5 → FXC → DXBC   *(veya doğrudan FXC uyumlu alt küme — spike karar verir)*
  - Vulkan: DXC → SPIR-V
  - Metal: DXC → SPIR-V → SPIRV-Cross → MSL (+ `xcrun metal` doğrulama CI'da)
  - GLES3 / WebGL2: DXC → SPIR-V → SPIRV-Cross → GLSL ES 3.00 / 3.20
  - WebGPU: DXC → SPIR-V → **Tint** → WGSL
- Offline derleme; çıktı `.shaderpack` (permutation key → backend blob). Runtime'da derleme yok (Web/GLES hariç driver-level link).
- **Spike kabul kriteri:** tek bir tam glTF PBR shader'ı (metal-roughness + normal + emissive + occlusion + skin + morph) tüm 6 hedefe derlenir; Windows'ta çalışan 3 backend'de (D3D11, D3D12, Vulkan) aynı görüntüyü verir (FLIP < eşik). Başarısızsa plan revize edilir — sonraki fazlar bu spike'a bağlıdır.

## 6. Kapasite çözümleme sistemi (merkez artefakt)

Her **özellik** için nihai durum:

```
effective(feature) = asset_uses(feature)
                   ∧ backend_supports(feature, DeviceCaps)
                   ∧ setting_enabled(feature, cvar/scalability)
```

Sonuç her zaman şu üçünden biri: **ON**, **FALLBACK(<belgelenmiş yöntem>)**, **OFF(<neden>)**. Neden metni log'a ve debug UI'daki capability tablosuna yazılır. Shader permutation key bu sonuçtan üretilir (bitmask) — key asset'in kullanmadığı özellikleri hiç içermez → permutation patlaması asset başına sınırlı kalır.

### 6.1 Backend tier'ları
| Tier | Backend'ler | Özet kapasite |
|---|---|---|
| **T0** | D3D11 FL10_0/10_1, GLES 3.0, WebGL2 | compute yok/sınırlı (FL10: CS4.x opsiyonel; GLES3.0/WebGL2: yok), storage buffer yok, sınırlı texture formatları, vertex texture fetch sınırlı |
| **T1** | D3D11 FL11, GLES 3.2, WebGPU | compute var, storage buffer var, BC (desktop) / ASTC-ETC2 (mobil) |
| **T2** | D3D12, Vulkan 1.2+, Metal 3 | explicit API, bindless opsiyonel, async compute, wave ops, upscaler'lar |
| **T2+RT** | D3D12 DXR 1.1, Vulkan RT, Metal RT | ray tracing extra'ları (opsiyonel, glTF dışı) |

### 6.2 Özellik × tier tablosu (ilk sürüm — Faz 0'da tamamlanır, her faz günceller)
| Özellik | T0 | T1 | T2 |
|---|---|---|---|
| Core PBR metal-roughness, normal/occlusion/emissive | ON | ON | ON |
| Alpha mask / blend, double-sided | ON | ON | ON |
| Skinning | FALLBACK: CPU skinning veya uniform-dizi kemik limiti (≤ N joint) | ON (GPU, storage buffer) | ON |
| Morph targets | FALLBACK: CPU blend veya ≤ 8 aktif target vertex-attrib | ON (storage buffer, sınırsız) | ON |
| KHR_materials_transmission / volume / dispersion | FALLBACK: tek-katman ekran kopyası, mip'siz blur yaklaşık | ON (transmission pass, mip chain) | ON |
| KHR_materials_clearcoat / sheen / specular / ior / iridescence / anisotropy / emissive_strength / diffuse_transmission / unlit | ON (shader-only; texture sayısı limitine göre FALLBACK) | ON | ON |
| KHR_texture_basisu (KTX2) | ON: transcode hedefi caps'e göre (BC / ETC2 / ASTC / RGBA8) | ON | ON |
| KHR_draco / EXT/KHR_meshopt / KHR_mesh_quantization | ON (CPU decode) | ON | ON |
| EXT_mesh_gpu_instancing | FALLBACK: draw başına instance-buffer (D3D11/GLES instancing var) | ON | ON |
| KHR_lights_punctual | ON (forward, ışık sayısı limitli) | ON (clustered) | ON (clustered) |
| IBL (image based lighting) | ON (önceden hesaplanmış, CPU/offline prefilter) | ON (GPU prefilter compute) | ON |
| Shadow map (CSM / spot / point) | ON (azaltılmış cascade) | ON | ON |
| TAA | OFF (compute yok) veya FALLBACK FXAA | ON | ON |
| Upscaler'lar | FSR1 (spatial, sadece pixel shader) | FSR1 / FSR2-3 analitik (compute) | DLSS / FSR / XeSS / MetalFX (bkz. §9) |
| Ray tracing extras (yansıma/gölge) | OFF | OFF | ON (T2+RT) |

Not: T0 tablosunun **gerçek** değerleri Faz 2'de D3D11 FL10_0 üzerinde ölçülerek doğrulanır; tablo varsayım değil, test sonucu olur.

## 7. glTF kapsamı

### 7.1 Core glTF 2.0 (hepsi zorunlu)
`.glb` + `.gltf` (embedded/external/data-URI buffer), tüm accessor tipleri + normalized + sparse accessor, tüm primitive mode'ları (POINTS, LINES, LINE_LOOP, LINE_STRIP, TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN — desteklemeyen API'de index'e dönüştürme), çoklu UV set, vertex color, tangent üretimi (MikkTSpace, yoksa), sampler (wrap/filter/mipmap), PNG/JPEG, node hiyerarşisi (TRS + matrix), çoklu scene, perspektif + ortografik kamera, skin (inverse bind matrices, çoklu skin), morph targets (POSITION/NORMAL/TANGENT + ek attribute'lar), animasyon (translation/rotation/scale/weights; LINEAR/STEP/CUBICSPLINE), material: metal-roughness, normal/occlusion/emissive, alphaMode (OPAQUE/MASK/BLEND) + alphaCutoff, doubleSided, default material, sınırsız-boyut uyarıları.

### 7.2 Extension'lar (**ön liste — Faz 0'da KhronosGroup/glTF registry'den doğrulanıp kesinleşir**)
KHR (ratified olanlar hedef): `KHR_draco_mesh_compression`, `KHR_lights_punctual`, `KHR_materials_anisotropy`, `KHR_materials_clearcoat`, `KHR_materials_diffuse_transmission`, `KHR_materials_dispersion`, `KHR_materials_emissive_strength`, `KHR_materials_ior`, `KHR_materials_iridescence`, `KHR_materials_sheen`, `KHR_materials_specular`, `KHR_materials_transmission`, `KHR_materials_unlit`, `KHR_materials_variants`, `KHR_materials_volume`, `KHR_mesh_quantization`, `KHR_meshopt_compression` (ratification durumu doğrulanacak), `KHR_texture_basisu`, `KHR_texture_transform`, `KHR_animation_pointer`, `KHR_xmp_json_ld` (metadata — okunur, render etkisi yok).

EXT (yaygın kullanılanlar): `EXT_meshopt_compression`, `EXT_mesh_gpu_instancing`, `EXT_texture_webp`, `EXT_texture_avif`, `EXT_lights_image_based`(opsiyonel, durum doğrulanacak).

Taslak/ratification bekleyenler (ör. `KHR_interactivity` — Temmuz 2026'da ratification'a sunuldu): **kapsam dışı**, yükleyici `extensionsRequired` içinde görürse anlamlı hata verir; `extensionsUsed` içindeyse yok sayıp uyarır.

### 7.3 Kabul kriteri ("tam kapsam" tanımı)
1. **Khronos glTF-Validator**: her test asset'i sıfır error ile valid → yükleyici bunları hatasız yükler; validator'ın reddettiği asset'leri yükleyici de anlamlı hatayla reddeder.
2. **Khronos glTF-Sample-Assets** setinin tamamı render edilir; her asset için **Khronos glTF-Sample-Viewer** (referans renderer) ile üretilen referans görüntüye karşı FLIP diff < eşik (eşik asset başına ayarlanabilir, gerekçeli).
3. Her extension için en az bir hedefli sample asset golden testi (ör. `ClearCoatTest`, `TransmissionTest`, `IridescenceLamp`, `AnimationPointerUVs` …).
4. Desteklenmeyen kombinasyon → OFF(neden) veya FALLBACK, asla sessiz yanlış render.

## 8. Settings sistemi (Unreal benzeri)

- **CVar** kayıt sistemi: `r.Shadow.Quality`, `r.Transmission.Enable`, `r.Upscaler.Mode`, `r.Upscaler.Quality`, `r.TAA.Enable`, `r.IBL.Resolution`, `r.Material.Clearcoat` … (her opsiyonel glTF extension + her render özelliği bir cvar).
- CVar özellikleri: tip (bool/int/float/string/enum), varsayılan, min/max, açıklama, flag'ler (`ReadOnly`, `RequiresRestart`, `RequiresPipelineRebuild`, `Cheat`), değiştirilme kaynağı (öncelik sırası: Default < Scalability < Platform ini < Project ini < User ini < CommandLine < Console/Code).
- **Scalability grupları** (`sg.ShadowQuality`, `sg.PostProcessQuality`, `sg.TextureQuality`, `sg.EffectsQuality`, `sg.AntiAliasingQuality`, `sg.ResolutionQuality`) — her grup 0..4 (Low/Medium/High/Epic/Cinematic); her seviye bir cvar seti.
- **ini katmanları**: `Config/Default*.ini`, `Config/<Platform>/<Platform>*.ini`, kullanıcı `Saved/Config/*.ini`.
- **Runtime değişim**: cvar değişince capability resolver tekrar çalışır; gereken permutation'lar yüklenir/derlenir (Web/GLES) veya pack'ten alınır; `RequiresRestart` olanlar bir sonraki açılışta.
- **Otomatik preset**: ilk açılışta DeviceCaps + basit GPU benchmark → önerilen scalability seviyesi (Unreal'ın "auto detect" karşılığı).
- Debug UI (ImGui): cvar konsolu, scalability slider'ları, capability tablosu (özellik × durum × neden).

## 9. Upscaler'lar

Ortak arayüz: `IUpscaler` (girdi: renk, depth, motion vector, exposure, jitter, reactive mask; çıktı: hedef çözünürlük). Renderer zaten TAA için jitter + motion vector üretir → tüm temporal upscaler'lar aynı girdiyi kullanır. Seçim: `r.Upscaler.Mode = None|FSR1|FSR|DLSS|XeSS|MetalFX|Auto`.

**Destek matrisi (web araştırması 2026-11-27; her upscaler fazının başında resmi SDK dokümanından tekrar doğrulanır):**

| Upscaler | D3D12 | D3D11 | Vulkan | Metal | GLES/WebGL2 | WebGPU | Donanım | Bu makinede çalışır mı |
|---|---|---|---|---|---|---|---|---|
| **DLSS** (Streamline / NGX) | ✔ | ✔ | ✔ (NGX/Streamline Vulkan yolu — doğrulanacak) | ✘ | ✘ | ✘ | NVIDIA RTX 20+; MFG sadece RTX 50 | ✔ SR (RTX 3070 Ti); FG/MFG ✘ |
| **FSR** (AMD FSR SDK 2.4; FSR 4.1.x ML) | ✔ | ✘ (resmi SDK'da yok) | ✔ (SDK 2.4 ile ML dahil) | ✘ | ✘ | ✘ | ML upscale: AMD RX 6000+/Ryzen APU; analitik FSR 3.x: tüm vendor'lar | analitik ✔; ML ✘ |
| **FSR 1 / FSR 2 açık kaynak shader portu** (MIT) | — | ✔ | — | ✔ | FSR1 ✔ | ✔ (FSR1; FSR2 compute port) | tüm vendor'lar | ✔ |
| **XeSS-SR** (XeSS SDK 2.1+) | ✔ | Arc'ta ✔; diğer vendor durumu kaynaklarda çelişkili — doğrulanacak | ✔ (SR only) | ✘ | ✘ | ✘ | Intel Arc (XMX) veya SM6.4 DP4a GPU | ✔ SR (DP4a) |
| **XeSS-FG / XeLL** | ✔ | ✘ (doğrulanacak) | ✘ | ✘ | ✘ | ✘ | SM6.4+ | ✔ (D3D12) |
| **MetalFX** (spatial + temporal) | — | — | — | ✔ | — | — | Apple Silicon / A-serisi | ✘ (Mac yok, K2) |

Kurallar:
- SDK yoksa / donanım desteklemiyorsa → capability OFF(neden), `Auto` modu sıradaki uygun upscaler'a düşer (öncelik: DLSS > XeSS(XMX) > FSR-ML > FSR analitik > XeSS(DP4a) > FSR1 > None; builder ayarlayabilir).
- Frame generation (DLSS-FG, FSR-FG, XeSS-FG): upscaling'den **sonraki** opsiyonel faz (swapchain entegrasyonu ayrı iş).

## 10. Doğrulama altyapısı (her fazın kalite kapısı)

- `apps/headless`: pencere olmadan, belirtilen backend'de, belirtilen asset + kamera + cvar setiyle render → PNG.
- `tools/golden`: referans görüntüyle **NVIDIA FLIP** (veya PSNR/SSIM) diff; HTML/MD rapor; eşik aşılırsa non-zero exit.
- Referanslar: (a) Khronos glTF-Sample-Viewer çıktısı (doğruluk referansı), (b) bizim D3D12 çıktımız (backend'ler arası tutarlılık referansı).
- Backend validation layer'ları test modunda zorunlu açık: D3D12 debug layer + GPU-based validation, D3D11 debug, Vulkan validation layers (sync validation dahil), WebGPU error scope'ları. **Validation hatası = test başarısız.**
- CI: GitHub Actions (repo push'u kullanıcı onayıyla): Windows (build + D3D11 WARP / D3D12 WARP headless — GPU'suz runner'da), Linux (Vulkan lavapipe/SwiftShader), macOS (Metal **sadece build + MSL derleme**, K2), Android (build), Web (build + opsiyonel headless Chrome WebGPU). Gerçek GPU testleri bu makinede yerel çalışır.

## 11. Fazlar

Her faz: **hedef → çıktılar → kapı (gate)**. Kapı geçilmeden sonraki faza geçilmez.

| Faz | Hedef | Kapı (reviewer kendisi çalıştırır) |
|---|---|---|
| **F0 — İskelet & doğrulama altyapısı** | git init, CMake preset'leri, core (log/assert/cvar çekirdeği), `headless` + `golden` araçları, glTF-Sample-Assets + Sample-Viewer referans görüntüleri, extension listesinin registry'den kesinleşmesi, worklist dosyası | build green; golden aracı bilinen iki PNG'yi doğru karşılaştırıyor; extension listesi registry linkiyle commit |
| **F1 — Shader hattı spike** (§5.4) | HLSL → 6 hedef; tek PBR shader | 6 hedefe derleme başarılı; MSL `xcrun metal` CI'da (K2 notu ile) |
| **F2 — RHI çekirdek: Vulkan + D3D11** | RHI arayüzü, iki backend, swapchain, buffer/texture/pipeline/bind group, üçgen → textured mesh; DeviceCaps; D3D11 FL10_0 ölçümü | headless üçgen + textured cube iki backend'de FLIP < eşik; validation temiz; T0 kapasite tablosu ölçülmüş |
| **F3 — glTF yükleyici + core PBR** | fastgltf, core glTF tam (§7.1), MikkTSpace, KTX2/Basis, Draco, meshopt, quantization; metal-roughness PBR + IBL + punctual lights + shadow | glTF-Sample-Assets core alt kümesi Vulkan+D3D11'de referansa karşı geçer |
| **F4 — Animasyon** | skin, morph, animasyon (3 interpolasyon), KHR_animation_pointer, T0 fallback'leri | animasyonlu sample asset'ler belirli zaman damgalarında golden geçer |
| **F5 — Capability resolver + permutation sistemi + settings** | §6 resolver, permutation key, `.shaderpack`, CVar/scalability/ini (§8), ImGui debug UI | resolver unit testleri; cvar değişimi → doğru permutation (test); tablo debug UI'da |
| **F6 — Material extension'ları** | §7.2'deki tüm KHR_materials_*, texture_transform, variants, transmission pass | her extension golden testi geçer (Vulkan + D3D11) |
| **F7 — D3D12 backend** | D3D12 (birincil Windows) | tüm golden setin D3D12'de geçmesi; D3D12 = backend-tutarlılık referansı |
| **F8 — Frame graph + TAA + post** | frame graph, TAA (jitter + motion vector), tonemap, bloom | TAA golden (deterministik jitter) |
| **F9 — Upscaler'lar (Windows)** | IUpscaler, FSR1 portu, FSR SDK (D3D12/Vulkan), DLSS (D3D11/12/VK), XeSS (D3D12/VK/D3D11*) | her biri bu makinede çalışır + görüntü sanity (eşik gevşek); desteklenmeyen combo OFF(neden) |
| **F10 — Web: WebGPU + WebGL2** | emsdk kurulumu, WebGPU backend, GLES/WebGL2 backend, browser viewer | headless Chrome WebGPU golden; WebGL2 T0 golden |
| **F11 — Android: Vulkan + GLES3** | NDK, NativeActivity, ASTC/ETC2, emulator | emülatörde golden (K3 notu) |
| **F12 — Linux** | X11/Wayland surface | Vulkan golden (lavapipe CI + yerel WSL opsiyonel) |
| **F13 — Metal (macOS + iOS)** | Metal backend, MetalFX | **sadece** derleme + MSL doğrulama (K2); runtime = DOĞRULANMADI |
| **F14 — Frame generation + RT extras (opsiyonel)** | DLSS-FG / FSR-FG / XeSS-FG, DXR/VK RT yansıma-gölge | bu makinede çalışan alt küme |
| **F15 — Gerçek Mac doğrulaması (ertelenmiş)** | kiralık Mac / kullanıcı Mac edinirse | Metal golden seti |

Fazlar arası bağımlılık: F0 → F1 → F2 → F3 → {F4, F5} → F6 → F7 → F8 → F9; F10/F11/F12 F6 sonrasında paralel yürüyebilir; F13 F7 sonrası herhangi bir zaman.

## 12. İş akışı (Opus 5.5 uygular, Fable 5.2 kontrol eder)

- Her faz `superpowers:writing-plans` ile ayrı bir **uygulama planına** (task listesi) dönüşür: `docs/superpowers/plans/YYYY-MM-DD-fNN-<konu>.md`.
- Her faz bir **Workflow** (veya Agent zinciri) olarak koşar, ~10 agent sınırı içinde:
  1. **Implementer** (Agent, `model: opus`) — plan task'larını uygular, testleri yazar, build'i yeşile getirir, commit eder.
  2. **Reviewer** (Agent, `model: fable`) — implementer'ın özetini **değil**, kodu (diff) ve artefaktları alır; kapıyı **kendisi çalıştırır**: build, headless render, golden diff, validation log. Çıktı: PASS / FAIL + bulgu listesi (dosya:satır).
  3. FAIL → bulgular implementer'a geri (en fazla N tur; sonra kullanıcıya eskalasyon).
  4. PASS → faz raporu + worklist güncellemesi + kullanıcıya özet.
- **Worklist** (`docs/worklist.md`): her task satırı: `fNN.tMM | durum (TODO/WIP/DONE/BLOCKED) | kanıt (commit, test adı, golden rapor yolu) | not`. BLOCKED satırı somut kanıt içerir (ör. "Mac yok, K2").
- Her faz başlamadan önce kullanıcıya tahmini maliyet/agent sayısı söylenir ve onay alınır.
- Reviewer "doğrulanmadı" ile "geçti"yi ayırır; Metal/iOS ve fiziksel Android için her raporda K2/K3 notu tekrar edilir.

## 13. Riskler

| Risk | Etki | Azaltma |
|---|---|---|
| Shader çapraz derleme (HLSL→MSL/WGSL/GLSL ES) özellik kayıpları | Fazları bloklar | F1 spike; sorunlu yapılar için shader yazım kuralları dokümanı |
| T0 kısıtları (compute yok, storage buffer yok) birçok özelliği zorlar | Fallback işi büyür | F2'de ölçüm; §6.2 fallback'leri belgelenmiş |
| Metal hiç çalıştırılamıyor | Apple'da hatalar geç bulunur | Erken CI derlemesi; F15 ertelenmiş doğrulama |
| Upscaler SDK lisans / API değişimi | Entegrasyon kırılır | SDK'lar opsiyonel CMake bağımlılığı; faz başında doküman tekrar doğrulanır |
| Permutation patlaması | Derleme süresi / boyut | Asset-güdümlü key; offline pack; cache |
| Kapsam büyüklüğü (6+ backend × tam glTF) | Süre | Faz kapıları; her faz bağımsız değer üretir |

## 14. Builder check sonrası kararlar (tasarımcının kararı — kullanıcı itiraz edebilir)

- **K-A Permutation modeli (§5.4/§6 çelişkisinin çözümü):** Runtime'da cross-compiler YOK. Tier başına **uber-shader** + kaba **feature-group** permutation'ları (ör. `base`, `+clearcoat/sheen/specular grubu`, `+transmission/volume grubu`, `+skin`, `+morph` … hedef ≤ 64 varyant/backend). Grup içindeki ince özellikler: Vulkan'da specialization constant, Metal'de function constant, D3D/GLES/WebGPU'da uniform-tabanlı dinamik branch. Tamamı offline `.shaderpack` içinde → herhangi bir runtime asset karşılanır. §8'deki "Web/GLES'te derlenir" ifadesi = sadece driver-level program link/pipeline oluşturma.
- **K-B Referans görüntüler:** "Reference mode" (`r.ReferenceMode=1`): Khronos glTF-Sample-Viewer ile aynı BRDF, IBL prefilter + LUT, Khronos PBR Neutral tonemap, aynı environment map, kamera ve exposure; shadow/TAA/bloom kapalı. Sample-Viewer referansları F0'da headless Chrome ile bir kez üretilip `tests/golden/ref/` altına commit edilir. Normal mod (shadow, post, upscaler) serbesttir; kendi D3D12 golden'larına karşı test edilir.
- **K-C Fallback kabulü:** T2 ve T1: reference mode'da Sample-Viewer'a karşı sıkı eşik. T0 FALLBACK: (a) kendi golden'ına karşı sıkı eşik (regresyon) + (b) Sample-Viewer'a karşı gevşek, gerekçeli eşik (sanity). "Tam glTF kapsamı" T1 ve T2'nin tamamında zorunlu; T0'da her özellik ON veya belgelenmiş FALLBACK olmalı, OFF sadece §6.2'de listelenenler.

- **K-D Kütüphane şekli (kullanıcı: "ileride başka bir uygulamayla birleşecek, önce stabil olmalı"):** Renderer bir kütüphanedir (`engine` static/shared lib); `apps/viewer` ve `apps/headless` onu sadece **public facade** üzerinden kullanır (`include/engine/*.h`: `Renderer`, `Scene`, `AssetHandle`, `Settings`, `Camera`). İç modüller serbestçe değişir; facade F7'den itibaren dondurulur (semver, değişiklik = changelog). Threading kontratı: tek "render thread" sahibi; asset yükleme job system'de asenkron, sonuç handle ile. Facade'a host-uygulama entegrasyonu için harici surface/native window handle verme ve harici device paylaşımı (opsiyonel) eklenir.
- **K-E Performans hedefleri (kullanıcı: "tüm cihazlarda yüksek fps"; sayılar tasarımcının önerisi):**
  - Masaüstü: orta sınıf GPU (RTX 3060 / RX 6600 sınıfı) 1440p `High` preset'te ≥ 60 fps, bu makinede (RTX 3070 Ti) ≥ 120 fps hedef; `Low` preset entegre GPU'da 1080p ≥ 60 fps.
  - Mobil: orta sınıf Android (Vulkan 1.1, Adreno 6xx / Mali-G7x sınıfı) `Medium` preset'te ≥ 60 fps; T0 (GLES 3.0) `Low` preset ≥ 30 fps.
  - Web: WebGPU `Medium` ≥ 60 fps (masaüstü tarayıcı).
  - Referans sahne: Sponza + 10 karakter (skin) + 64 punctual light + 1 directional (CSM). Her fazdan sonra headless frame-time ölçümü (`apps/headless --bench`) worklist'e yazılır; %10'dan büyük regresyon = kapı FAIL.
  - Varsayılan limitler (değiştirilebilir cvar): T0 ≤ 64 joint/skin, ≤ 8 aktif morph target, ≤ 8 ışık/obje (forward); T1/T2 clustered ≤ 1024 ışık.
- **K-F Işık kalite seviyeleri (kullanıcı: "kesinlikle olsun"):** Ayrı scalability grubu `sg.LightingQuality` (0..4) + mevcut `sg.ShadowQuality`. Kapsadığı cvar'lar: `r.Lighting.MaxLights`, `r.Lighting.Clustered`, `r.Lighting.Specular` (punctual specular aç/kapa), `r.IBL.Resolution`, `r.IBL.SpecularSamples`, `r.IBL.DiffuseMode` (SH / irradiance map), `r.Shadow.CascadeCount`, `r.Shadow.MapResolution`, `r.Shadow.Filter` (Hard/PCF/PCSS), `r.Shadow.PointLights`, `r.Shadow.Distance`, `r.Transmission.Quality`, `r.AO.Mode` (glTF occlusion only / +SSAO). Low→Cinematic tablosu F5'te yazılır, her seviyenin frame-time'ı referans sahnede ölçülür.
- **Repo:** Private GitHub repo (kullanıcı onayı, 2026-11-27); CI GitHub Actions — macOS dakikaları private repoda ücretli/kotalı, sadece Metal derleme job'u macOS'ta koşar.

## 15. Builder'a bırakılanlar
- Math kütüphanesi (kendi vs. glm/DirectXMath), job system tasarımı, bellek allocator'ları (VMA / D3D12MA kullanımı önerilir).
- Frame graph iç tasarımı.
- Golden diff eşik değerleri (asset başına, gerekçeli).
- `Auto` upscaler öncelik sırasının ince ayarı.
- FetchContent vs vcpkg.
- D3D11 shader yolu (DXC→SPIRV-Cross→FXC vs. doğrudan FXC alt kümesi) — F1 spike karar verir.
