# Plan de modernización del template

> ESTADO: las fases 1, 2 y 3 están **implementadas** (ver abajo). Lo marcado como
> *best-effort* compila/está cableado pero necesita una pasada en su plataforma real
> (iOS en un Mac, BSD/RISC-V en CI) para afinar detalles.

## Estado actual (ya implementado)
- Fixes de bugs (ruta de recursos/conejo, target `assembler`, JNI de raymob, zips de CI, etc.).
- **Refactor de ciclo de vida estilo Godot** en `src/main.cpp`: el juego vive en
  `_ready()` / `_process()` / `_exit()`, y un "runner" de plataforma los conduce:
  - Desktop / BSD / Android / Web (`-s ASYNCIFY`): bucle `main()` clásico.
  - iOS: callbacks `ios_ready/ios_update/ios_destroy` (lo exige el fork, ver Fase 3).
  - Verificado: compila y el conejo carga.
- **rres (Fase 1):** headers vendorizados en `thirdparty/rres/`, capa de assets dual
  (`include/assets.h` + `src/assets.cpp` + `src/assets_rres.c`), empaquetador abierto
  `tools/rres_pack` y targets CMake `pack_resources`/`unpack_resources`.
  **Verificado en local:** modo suelto, `.rres` sin cifrar y `.rres` **AES** descifran bien.
- **Versiones congeladas:** raylib 6.0.0 (snapshot commiteado) y raylib-iOS fijado como
  submódulo en el tag `6.0.3-iOS` (ver `thirdparty/FROZEN_VERSIONS.md`).
- **CI/CD (Fase 2):** imagen Docker congelada en su **propio repo** (`../raylib-build-image/`:
  `Dockerfile` + `docker-image.yaml` → GHCR) y `.github/workflows/build.yaml` multiplataforma:
  Linux x64/ARM64/RISC-V, Windows x64/ARM64, macOS, iOS, Web, Android, FreeBSD/OpenBSD/NetBSD.
  **Acciones fijadas por SHA y todas en Node 22+** (GitHub depreca Node 20): checkout v7,
  upload-artifact v7, download-artifact v8, cache v6, docker/* en sus majors node24,
  cross-platform-actions v1.3.0 y action-gh-release v3. Se quitó `ilammy/msvc-dev-cmd`
  (sigue en Node 20) y se sustituyó por `vcvarsall` inline para Windows ARM64.
- **iOS (Fase 3):** submódulo del fork + scaffold XcodeGen en `ios/` + job CI que compila
  `raylib.xcframework` (ver `ios/README.md`). *Best-effort: afinar en un Mac.*

---

## Investigación: hallazgos clave

### rres — cifrado, coste en runtime y empaquetador
- El **lector** (`rres.h` + `rres-raylib.h`) es **libre y open-source**, y funciona en
  cualquier plataforma (Linux local, Windows, BSD...). **No necesitas comprar nada para
  CARGAR un `.rres` en runtime.**
- Lo único de pago/cerrado es **`rrespacker`** (crear el `.rres`), ~$19.95 en itch.io.
- **Cifrado AES (formato exacto que espera `rres-raylib.h`)** — investigado byte a byte:
  - Layout del chunk cifrado: `[datos_cifrados][salt(16)][MD5(16)]`; `packedSize` incluye los 32 bytes extra.
  - Clave: **Argon2i** (16 MB / 3 pasadas / 1 lane) sobre `contraseña + salt` → clave de 32 bytes.
  - Cifrado: **AES-256 en modo CTR con IV todo-ceros** (`AES_init_ctx` deja el `Iv` a cero).
  - Integridad: **MD5** del texto claro almacenado al final (hay que comprobarlo al descifrar).
  - **Ojo:** `rres-raylib.h` llama a `ComputeMD5()`, que **no define** — hay que proporcionar
    una implementación de MD5 para que la ruta AES compile.
- **¿Penalización en runtime?** **No por frame.** El descifrado/descompresión ocurre solo al
  **cargar** cada recurso. El coste dominante es el **Argon2i por recurso** (decenas de ms cada
  uno); con pocos assets es despreciable, con cientos puede sumar segundos al arranque. Se puede
  mitigar cargando en hilos o en pantallas de carga. AES-CTR y MD5 en sí son rápidos.
- **rres local vs Docker:** en tu Linux local puedes **usar/cargar** rres sin problema (el lector
  es libre). Lo que queda ligado a Docker (o a comprar rrespacker) es **generar** el `.rres`.
  Para no depender del binario de pago, lo recomendado es un **empaquetador propio** (ver decisión D1).

### iOS — el fork `ghera/raylib-iOS` es el camino
- Añade `src/platforms/rcore_ios.c` (basado en el PR #3880). El autor publica juegos en la App Store con él.
- **Gráficos vía ANGLE** (OpenGL ES → Metal), porque iOS ya no expone GLES nativo. El fork trae
  xcframeworks de ANGLE precompilados en `deps/ANGLE/`.
- **Ciclo de vida por callbacks** (sin bucle while): `ios_ready()`, `ios_update(bool)`, `ios_destroy()`,
  invocados desde `rcore_ios.c`. `main()` lo define raylib (`UIApplicationMain`). **Este es el motivo
  del refactor Godot ya implementado.**
- **Build NO es CMake**: raylib se compila como **xcframework** con `projects/scripts/build-ios-xcframework.sh`
  (clang + xcrun, slices device-arm64 + simulator), y la app es un **proyecto Xcode** (`projects/Xcode26`)
  que enlaza raylib.xcframework + ANGLE. Requiere **macOS + Xcode** (no se puede hacer en Docker/Linux).
- El **CMake del fork no contempla iOS**; iOS va por Xcode. Hay utilidades extra: `GetIOSSafeAreaInsets()`,
  `GetIOSDocumentsPath()` (`IOSBridge`).

### Multiplataforma (Windows/Linux ARM, BSDs, RISC-V) — viabilidad CI
- raylib es C99 portable: compila en FreeBSD (está en ports), y en general en los BSD y RISC-V
  si tienes GLFW/X11/OpenGL del sistema. El límite no es raylib, son los **runners y las toolchains**.
- **Runners hospedados por GitHub Actions:** Ubuntu x64 y **ARM64** (`ubuntu-24.04-arm`), Windows x64
  (y ARM64 vía cross-compile MSVC), macOS x64/ARM64. **No hay runners BSD ni RISC-V.**
- Consecuencia práctica (matriz):
  - **Linux x64 / ARM64:** directo en runners hospedados (o dentro de la imagen Docker).
  - **Linux RISC-V:** cross-compile desde la imagen Docker (`riscv64-linux-gnu`); probar requiere
    `qemu-user` o hardware real.
  - **Windows x64 / ARM64:** runner Windows; ARM64 por cross-compile MSVC (o runner arm64 si está disponible).
  - **macOS:** runner hospedado. **iOS:** runner macOS + Xcode (Fase 3).
  - **FreeBSD / OpenBSD / NetBSD:** **sin runner hospedado**, pero se cubren con la acción
    **`cross-platform-actions/action`**: arranca una VM **QEMU** del BSD elegido dentro de un runner
    `ubuntu-latest` (GitHub hospedado, **sin self-hosted**). Soporta:
    - FreeBSD: x86-64, arm64 y **riscv64** (solo 15.x)
    - OpenBSD: x86-64, arm64
    - NetBSD: x86-64, arm64
    - Uso: `operating_system: freebsd|openbsd|netbsd`, `version`, `architecture`, y dentro instalas
      dependencias (`pkg install ...`) y compilas. Ojo: es lento (arranque de VM + instalación cada run)
      y, al no haber display, el objetivo realista es **compilar+enlazar** (y smoke-test), no ejecutar el juego.
  - **RISC-V Linux:** no es un huésped de esa acción; se hace con cross-compile + **qemu-user**
    (`docker/setup-qemu-action` o `riscv64-linux-gnu-gcc`) en la imagen Docker.

---

## Fase 1 — rres (empaquetado + cifrado por defecto)

1. Vendorizar en `thirdparty/rres/`: `rres.h`, `rres-raylib.h` y `src/external/` (aes, monocypher, lz4, qoi).
2. Añadir `include/assets.h` + `src/assets.cpp`:
   - Producción: abrir `resources.rres`, cargar el **directorio central** y exponer
     `LoadTextureRres("rabbit.png")` (resuelve nombre→id→chunk→`Texture2D`).
   - Desarrollo: cargar ficheros sueltos (iteración rápida, sin empaquetador local).
   - Cifrado por defecto: `rresSetCipherPassword(...)` antes de cargar.
   - Definir `RRES_IMPLEMENTATION`/`RRES_RAYLIB_IMPLEMENTATION` en una sola TU, más
     `RRES_SUPPORT_ENCRYPTION_AES`; **proporcionar `ComputeMD5`** (necesario para AES).
3. **Empaquetador** (decisión D1): objetivo = target CMake `pack_resources` que genere `resources.rres`
   (con AES) durante el build, **dentro de la imagen Docker y también en local**, sin binario de pago.
   El formato AES a reproducir es el de arriba (Argon2i 16MB/3pass, AES-CTR IV cero, MD5). Se
   implementa reutilizando monocypher + tiny-AES ya incluidos y una pequeña MD5. **Hay que validarlo
   con un round-trip** contra el descifrado de `rres-raylib.h`.
4. Adaptar el runner/_ready para cargar vía assets (dev suelto / prod `.rres`).

## Fase 2 — CI/CD: UNA imagen Docker + matriz multiplataforma

- **Un solo repo de imagen** (`raylib-build-image`) con **todo lo posible**: toolchains C/C++,
  CMake+Ninja fijos, ccache, libs X11/Mesa, y además **emsdk** (Web), **Android SDK/NDK** (Android),
  **cross-compilers** `aarch64-linux-gnu` y `riscv64-linux-gnu` (ARM/RISC-V Linux) y, si se opta por
  el empaquetador propio, las deps para compilarlo. Publicar en **GHCR** con tag inmutable (sha256).
- Workflow principal:
  - Jobs Linux (x64/ARM64, y cross de RISC-V) **dentro del contenedor** (`container:` + digest).
  - Windows (x64/ARM64) y macOS: runners nativos con **caché**, versiones fijadas y **acciones por SHA**.
  - Web y Android: dentro de la imagen (lleva emsdk/NDK) o runners con caché.
  - Artefacto final: binario + `resources.rres` (ya no la carpeta `resources/` suelta).
- **BSD** (decisión D3): elegir entre self-hosted runners, Cirrus/SourceHut, o cross-compile. Sin una
  de estas, los BSD no se pueden buildar en GitHub Actions.

## Fase 3 — iOS (fork `raylib-iOS`)

1. **Decisión D2:** usar el fork `ghera/raylib-iOS` como raylib para iOS. Opción recomendada: mantener
   el raylib actual para desktop y **añadir el fork como origen de iOS** (submódulo/copia), porque el
   build de iOS no pasa por el CMake del template.
2. Empaquetar el juego como app Xcode: enlazar `raylib.xcframework` (build-ios-xcframework.sh) + ANGLE.
3. El juego ya es compatible gracias al refactor (`_ready/_process/_exit` → `ios_ready/ios_update/ios_destroy`).
4. Job `build-ios` en runner **macOS** (Xcode). Manejar `Info.plist`, safe-area (`GetIOSSafeAreaInsets`).
5. Verificar en simulador y luego device (audio/GLES vía ANGLE).

---

## Decisiones tomadas

- **D1 — Empaquetador rres: `rrespacker` dentro de la imagen Docker.** El binario lo aporta el usuario
  (lo compra y lo mete en su imagen). **Cifrado por defecto.** Consecuencias:
  - La capa de assets debe soportar **dos modos**: si existe `resources.rres` se carga de ahí (cifrado);
    si no, se cargan los ficheros sueltos (fallback). Así **funciona ya** aunque aún no esté rrespacker.
  - En CI, el paso de empaquetar se ejecuta **solo si rrespacker está disponible** en la imagen; si no,
    el artefacto lleva `resources/` suelta. Cuando añadas el binario, pasa a `.rres` automáticamente.
  - **Aviso de licencia:** rrespacker es de pago/cerrado. Para un template **público** no se debe
    commitear ni redistribuir el binario en una imagen GHCR pública: se integra solo en tu imagen/uso privado.
  - Local: para regenerar el `.rres` en tu Linux necesitarás tener rrespacker (lo comprarás). Mientras,
    en dev usas ficheros sueltos (sin empaquetador).
- **D2 — raylib para iOS: fork `ghera/raylib-iOS` solo para el target iOS**, manteniendo el raylib actual
  para desktop/Android/Web. El build iOS es Xcode/xcframework (no CMake) en runner macOS.
- **D3 — BSD y RISC-V en CI: todo en runners hospedados de GitHub Actions** (template público, sin
  self-hosted): los 3 BSD vía **`cross-platform-actions/action`** (VMs QEMU), y RISC-V Linux vía
  cross-compile + qemu-user en la imagen Docker. Cobertura: FreeBSD x64/ARM64/RISC-V(15.x),
  OpenBSD x64/ARM64, NetBSD x64/ARM64, Linux RISC-V. Los jobs BSD compilan+enlazan (sin display).
