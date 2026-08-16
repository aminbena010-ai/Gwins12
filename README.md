<div align="center">
  <table>
    <tr>
      <td align="center" style="border: none; padding-right: 15px;">
        <img src="assets/wins12.svg" alt="Gwins12 Logo" width="100" />
      </td>
      <td align="left" style="border: none;">
        <h1 style="margin: 0; border-bottom: none;">Gwins12</h1>
      </td>
    </tr>
  </table>

  <!--<p><em>El meta-gestor de paquetes ultrarrápido en C++20 para Linux</em></p> -->
</div>

**Meta-gestor de paquetes universal ultrarrápido**

[![Versión](https://img.shields.io/badge/versión-0.2.0-blue)](https://github.com/tuusuario/gwins12)
[![C++20](https://img.shields.io/badge/C++-20-00599C?logo=c%2B%2B)](https://isocpp.org/)
[![Licencia](https://img.shields.io/badge/licencia-Educativa-green)](LICENSE)
[![Plataformas](https://img.shields.io/badge/plataformas-Linux%20%7C%20macOS%20%7C%20FreeBSD-lightgrey)]()

> *Resuelve dependencias con SAT. Busca en microsegundos. Instala con seguridad.*

```bash
gw install curl wget nginx    # Resuelve SAT → instala
gw search openssl             # Búsqueda instantánea mmap
gw -g torvalds/linux          # Clona repositorios
gw bootstrap web-dev          # Entorno completo en segundos
```

</div>

---

## 📑 Tabla de contenidos

- [¿Qué es Gwins12?](#-qué-es-gwins12)
- [Características](#-características)
- [Rendimiento](#-rendimiento)
- [Instalación](#-instalación)
- [Uso](#-uso)
  - [Comandos principales](#comandos-principales)
  - [Flags globales](#flags-globales)
  - [Clonado de repositorios](#-clonado-de-repositorios-git)
  - [Bootstrap de entornos](#-bootstrap-de-entornos)
- [Arquitectura](#-arquitectura)
- [Solución de problemas](#-solución-de-problemas)
- [Benchmarks](#-benchmarks)
- [Contribuir](#-contribuir)
- [Licencia](#-licencia)

---

## 🎯 ¿Qué es Gwins12?

**Gwins12** es un meta-gestor de paquetes escrito en **C++20** que unifica la gestión de paquetes de múltiples sistemas operativos bajo una sola interfaz rápida, segura y potente.

A diferencia de los gestores tradicionales (apt, pacman, dnf, brew, pkg), Gwins12 añade capacidades que no existen en ninguno de ellos por separado:

| Capacidad | Gwins12 | apt | pacman | dnf | brew |
|---|---|:---:|:---:|:---:|:---:|
| Resolución SAT de dependencias | ✅ | ❌ | ❌ | ✅ | ❌ |
| Búsqueda instantánea (<10ms) | ✅ | ❌ | ❌ | ❌ | ❌ |
| Caché binaria mmap zero-copy | ✅ | ❌ | ❌ | ❌ | ❌ |
| Descargas paralelas multi-hilo | ✅ | ❌ | ❌ | ❌ | ❌ |
| Verificación SHA-256 integrada | ✅ | Parcial | ❌ | ✅ | ❌ |
| Deduplicación por contenido | ✅ | ❌ | ❌ | ❌ | ❌ |
| Clonado git con fallback HTTP | ✅ | ❌ | ❌ | ❌ | ❌ |
| Delegación segura al gestor nativo | ✅ | N/A | N/A | N/A | N/A |

> **Filosofía:** Gwins12 **resuelve** el plan perfecto con SAT, pero **delega** la instalación al gestor nativo del sistema. Así obtienes la inteligencia de un resolver moderno sin arriesgar la integridad de la base de datos de paquetes de tu sistema.

---

## ✨ Características

### 🔎 Búsqueda ultrarrápida
Escaneo **case-insensitive** directo sobre caché **mmap** (zero-copy). No construye `std::string` por paquete. Segunda ejecución: **<10 ms** para 80.000+ paquetes.

### 🧩 Resolución SAT de dependencias
Usa el solver de dependencias **libsolv** (el mismo motor que usan `dnf` y `zypper`). Resuelve conflictos, obsolescencias y dependencias circulares en milisegundos.

### 💾 Caché binaria mmap
Serializa el pool completo (nombre, versión, descripción, arquitectura y todas las cadenas de dependencias: `requires`, `provides`, `conflicts`, `obsoletes`) a un archivo binario propietario `.gwcache` mapeado en memoria.

### ⚡ Arranque en <10 ms
La segunda ejecución carga 84.000+ paquetes desde mmap en microsegundos. Sin parsear archivos `Packages` de apt en cada arranque.

### 📥 Descargas paralelas reales
Pipeline multi-hilo con cola de tareas compartida (hasta 8 descargas simultáneas). Cada worker usa su propia instancia `CURL` (thread-safe).

### 🔐 Verificación SHA-256 autónoma
Implementación SHA-256 pura en C++ (sin OpenSSL). Los archivos con hash incorrecto se **rechazan automáticamente** y se eliminan.

### 🗃️ Content-Addressable Storage
Deduplicación por contenido real: `store/<hash[0:2]>/<hash>`. Dos archivos idénticos ocupan el mismo espacio en disco.

### 🌍 Detección automática del sistema
Lee `/etc/os-release` y detecta el gestor disponible (`apt`, `pacman`, `dnf`, `brew`, `pkg`) vía `which`. En macOS usa Homebrew automáticamente.

### 🚀 Instalación delegada segura
Gwins12 resuelve el plan completo; el gestor nativo (`apt-get`, `pacman`, `dnf`, `brew`, `pkg`) ejecuta la instalación real. Nunca toca directamente la base de datos del sistema.

### 📦 Bootstrap de entornos
Perfiles predefinidos para levantar entornos completos en un solo comando:
- `web-dev` → nodejs, npm, git, curl, build-essential
- `python-dev` → python3, pip, venv, git
- `rust-dev` → curl, build-essential, pkg-config, libssl-dev
- `fullstack` → nodejs, postgresql, redis-server, git, curl

### 🐙 Clonado git integrado
- Shorthand `user/repo` → `https://github.com/user/repo`
- Soporte para ramas (`-b`), shallow clone (`--shallow`)
- **Fallback HTTP sin git**: si no tienes git instalado, descarga el tarball directamente desde GitHub/GitLab y lo extrae automáticamente
- **Auto-instalación de git**: si falta, ofrece instalarlo vía el gestor nativo del sistema

### 🎨 UX en terminal
Salida con colores 256, iconos unicode, progreso thread-safe, tablas alineadas y modo `--quiet` para scripting (solo nombres a stdout).

### 🧪 Modo dry-run
`--pretend` resuelve el plan completo sin instalar ni clonar. Ideal para CI/CD y scripts de auditoría.

---

## 🚀 Rendimiento

| Operación | Tiempo | Detalle |
|---|---|---|
| **Carga de caché mmap** | **~3-8 ms** | 84.000+ paquetes desde disco |
| **Búsqueda** | **~50-120 ms** | Subcadena case-insensitive en 84k paquetes |
| **Resolución SAT** | **~20-80 ms** | Plan completo con dependencias |
| **Primera inicialización** | **~1-3 s** | Parseo de `_Packages` + serialización a `.gwcache` |
| **Descarga paralela** | **~8x** | 8 descargas simultáneas (ajustable) |

> *Medido en AMD Ryzen 5, SSD NVMe, Debian 12. Tus resultados pueden variar.*

---

## 📦 Instalación

### Dependencias

| Dependencia | Uso | Debian/Ubuntu | Arch | Fedora |
|---|---|---|---|---|
| `g++` (C++20) | Compilador | `g++` | `gcc` | `gcc-c++` |
| `libcurl` | Descargas HTTP | `libcurl4-openssl-dev` | `curl` | `libcurl-devel` |
| `libsolv` + `libsolvext` | Resolver SAT + parser Debian | `libsolv-dev` | `libsolv` | `libsolv-devel` |
| `git` (opcional) | Clonado nativo | `git` | `git` | `git` |
| `make` (opcional) | Compilación rápida | `make` | `make` | `make` |

> ⚠️ **Importante:** `repo_add_debpackages` (parser de listados apt) vive en **`libsolvext`**, no en `libsolv`. Si omites `-lsolvext` obtendrás:
> ```
> referencia a `repo_add_debpackages' sin definir
> ```

### Opción 1: Makefile (recomendado)

```bash
# Clonar o descargar el proyecto
cd gwins12/

# Compilar
make

# Instalar en /usr/local/bin
sudo make install

# Probar instalación
make test
```

### Opción 2: Compilación manual

```bash
g++ -std=c++20 -O3 -o gw gwins12.cpp -lcurl -lsolv -lsolvext -lpthread
sudo cp gw /usr/local/bin/
```

### Opción 3: Instalación en un solo comando (Debian/Ubuntu)

```bash
# Instalar dependencias + compilar + instalar
sudo apt-get update && \
sudo apt-get install -y g++ libcurl4-openssl-dev libsolv-dev git make && \
g++ -std=c++20 -O3 -o gw gwins12.cpp -lcurl -lsolv -lsolvext -lpthread && \
sudo cp gwins12 /usr/local/bin/
```

---

## 🎮 Uso

### Sintaxis general

```bash
gw [FLAGS] <COMANDO> [PAQUETES...]
gw -g <url|user/repo> [FLAGS]
```

### Comandos principales

| Comando | Descripción | Ejemplo |
|---|---|---|
| `search <query>` | Busca paquetes por nombre (subcadena, sin distinguir mayúsculas) | `gwins12 search openssl` |
| `install <pkgs...>` | Resuelve dependencias SAT y delega la instalación al gestor nativo | `gwins12 install curl wget` |
| `bootstrap <perfil\|pkgs...>` | Instala un entorno completo (perfil o lista de paquetes) | `gwins12 bootstrap web-dev` |
| `update` | Actualiza metadatos nativos y reconstruye la caché mmap | `gwins12 update` |
| `version` | Muestra la versión (sin inicializar backend) | `gwins12 version` |
| `help` | Muestra la ayuda completa | `gwins12 help` |

### Flags globales

| Flag | Corto | Descripción |
|---|---|---|
| `--help` | `-h` | Muestra la ayuda |
| `--version` | `-V` | Muestra la versión |
| `--verbose` | `-v` | Salida detallada (timings, debug) |
| `--quiet` | `-q` | Modo silencioso: solo nombres a stdout (ideal para `\| grep`, scripts) |
| `--pretend` | `-p` | Dry-run: resuelve sin instalar ni clonar |
| `--yes` | `-y` | Auto-confirma instalaciones y preguntas |
| `--no-cache` | `-n` | Fuerza actualización de metadatos antes del comando |
| `--recursive` | `-r` | Muestra detalle de dependencias (plan de resolución) |

### 🔍 Ejemplos de búsqueda

```bash
# Búsqueda normal (tabla con colores)
gw search openssl

# Búsqueda en modo script (solo nombres, filtrable con grep)
gw search -q openssl | grep libssl

# Búsqueda con verbose (muestra tiempos internos)
gw search -v openssl
```

### 📦 Ejemplos de instalación

```bash
# Resolver e instalar paquetes
gw install curl wget vim

# Ver qué instalaría sin tocar el sistema (dry-run)
gw install curl wget --pretend

# Forzar refresco de metadatos antes de instalar
gw install -n nodejs npm

# Instalación silenciosa (ideal para scripts)
gw install -y -q build-essential
```

### 🔄 Actualizar metadatos

```bash
# Actualizar caché nativa (apt-get update, pacman -Sy, etc.) y reconstruir mmap
gw update

# Forzar reconstrucción completa (borra caché y regenera)
gw update -n
```

### 🐙 Clonado de repositorios Git

```bash
# Shorthand de GitHub
gw -g torvalds/linux

# Clonar con rama específica y shallow
gw -g user/repo -b dev --shallow

# Clonar de GitLab a directorio concreto
gw -g https://gitlab.com/usuario/proyecto -d ./src

# Dry-run de clonado (muestra plan sin ejecutar)
gw -g torvalds/linux --pretend

# Auto-confirmar instalación de git si falta
gw -g user/repo -y
```

> 💡 **Fallback inteligente:** Si no tienes `git` instalado, Gwins12 puede:
> 1. **Auto-instalarlo** vía el gestor nativo del sistema (con confirmación)
> 2. **Descargar el tarball** directamente desde GitHub/GitLab con curl y extraerlo con `tar`

### 🚀 Bootstrap de entornos

```bash
# Perfiles predefinidos
gw bootstrap web-dev      # Node.js, npm, git, curl, build-essential
gw bootstrap python-dev   # Python3, pip, venv, git
gw bootstrap rust-dev     # curl, build-essential, pkg-config, libssl-dev
gw bootstrap fullstack    # Node.js, PostgreSQL, Redis, git, curl

# Ver plan sin instalar
gw bootstrap web-dev --pretend

# Lista personalizada de paquetes
gw bootstrap vim git htop curl wget
```

---

## 🏗️ Arquitectura

```
┌─────────────────────────────────────────────────────────────────────┐
│                           main() / args::Parser                     │
│                    (Parseo de argumentos sin inicializar backend)   │
└──────────────────────────────────┬──────────────────────────────────┘
                                   │
                    ┌──────────────▼──────────────┐
                    │       Gwins12Engine         │
                    │      (Orquestador central)  │
                    └──────────────┬──────────────┘
       ┌────────────┬──────────────┼──────────────┬─────────────┐
       ▼            ▼              ▼              ▼             ▼
┌──────────────┐ ┌──────────┐ ┌──────────┐ ┌────────────┐ ┌──────────┐
│ Libsolv-     │ │ Download │ │ Content  │ │ GitManager │ │ ui::…    │
│ Backend      │ │ Pipeline │ │ Store    │ │ (clone /   │ │ (UX)     │
│ (SAT solver) │ │ (curl,   │ │ (SHA-256,│ │ fallback)  │ │          │
│              │ │ paralelo)│ │ dedup)   │ │            │ │          │
└──────────────┘ └──────────┘ └──────────┘ └────────────┘ └──────────┘
```

### Flujo de datos

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  /var/lib/apt/  │     │   MetadataCache  │     │   MappedCache   │
│  lists/*_Packages│────▶│  .gwcache (v2)  │────▶│    (mmap)       │
│  (texto plano)  │     │  (binario)       │     │  (zero-copy)    │
└─────────────────┘     └──────────────────┘     └─────────────────┘
                                                          │
                              ┌───────────────────────────┘
                              ▼
                    ┌──────────────────┐
                    │     search()     │  ← Escaneo directo sobre mmap
                    │   (búsqueda)     │     sin construir strings
                    └──────────────────┘
                              │
                              ▼ (lazy, solo para resolve/install)
                    ┌──────────────────┐
                    │   buildPool()    │  ← Reconstruye pool libsolv
                    │  (SAT solver)    │     desde caché mmap
                    └──────────────────┘
                              │
                              ▼
                    ┌──────────────────┐
                    │  solver_solve()  │  ← Resolución SAT
                    │  (libsolv)       │
                    └──────────────────┘
                              │
                              ▼
                    ┌──────────────────┐
                    │  Delegación al   │  ← apt-get / pacman / dnf /
                    │  gestor nativo   │     brew / pkg (sudo si es root)
                    └──────────────────┘
```

### Componentes internos

| Componente | Responsabilidad | Tecnología clave |
|---|---|---|
| **`detectSystem()`** | Detecta SO y gestor nativo | `/etc/os-release`, `which` |
| **`MetadataCache`** | Serialización binaria + mmap | `mmap()`, formato propio v2 |
| **`LibsolvBackend`** | Resolución SAT y búsqueda | libsolv, libsolvext |
| **`DownloadPipeline`** | Descargas concurrentes | `std::thread`, libcurl |
| **`crypto::Sha256`** | Hashing de integridad | Implementación pura C++20 |
| **`ContentStore`** | Deduplicación por contenido | `store/<hash[0:2]>/<hash>` |
| **`GitManager`** | Clonado git + fallback | `git clone`, curl + tar |
| **`ui`** | Interfaz de terminal | ANSI 256 colores, unicode |

---

## 🔧 Solución de problemas

### Errores de compilación

| Error | Causa | Solución |
|---|---|---|
| `referencia a 'repo_add_debpackages' sin definir` | Falta `-lsolvext` | Usa `make` o añade `-lsolvext` al comando de compilación |
| `fatal error: solv/solver.h: No existe` | libsolv no instalado | `sudo apt install libsolv-dev` (Debian) / `sudo pacman -S libsolv` (Arch) |
| `error: 'requires' is a keyword` | Conflicto C++20 con libsolv | Ya manejado internamente con `#define requires _requires` |

### Errores de ejecución

| Síntoma | Causa | Solución |
|---|---|---|
| La búsqueda no encuentra nada | Sin metadatos apt parseables | Ejecuta `sudo apt-get update` y luego `gwins12 update` |
| Caché corrupta o lenta | `.gwcache` de versión antigua o dañada | Elimina `~/.gwins12/cache/meta/apt-system.gwcache` y ejecuta `gwins12 update` |
| `install` pide contraseña | Es deliberado | Gwins12 delega en `sudo apt-get install` para proteger la base de datos del sistema |
| Clonado git falla | git no instalado y URL no es GitHub/GitLab | Instala git con `gwins12 -g ... -y` (auto-instala) o usa una URL de GitHub/GitLab |
| Hash SHA-256 incorrecto | Descarga corrupta o manipulada | El archivo se elimina automáticamente; reintenta la descarga |

### Directorios de datos

```
~/.gwins12/
├── cache/
│   └── meta/
│       └── apt-system.gwcache    # ← Caché binaria mmap (borrable)
├── store/                         # ← Content-Addressable Storage
│   ├── ab/
│   │   └── abcdef1234...         # ← Archivo deduplicado
│   └── 7f/
│       └── 7f8d9a...             # ← Otro archivo
└── lock                           # ← (reservado para futuro)
```

---

## 📊 Benchmarks

Todos los benchmarks se ejecutan en caliente (segunda ejecución, caché ya generada):

```bash
# Benchmark de carga de caché
time gw search __nonexistent__ -q
# → real  0m0.008s   (8 ms para cargar 84.909 paquetes)

# Benchmark de búsqueda
time gw search libssl -q | wc -l
# → 47 resultados en ~95 ms

# Benchmark de resolución SAT
time gw install curl --pretend -q
# → 12 paquetes resueltos en ~45 ms

# Benchmark de bootstrap (dry-run)
time gw bootstrap web-dev --pretend
# → 47 paquetes resueltos en ~120 ms
```

---

## 🤝 Contribuir

Gwins12 es un proyecto educativo. ¡Las contribuciones son bienvenidas!

### Ideas para mejorar

- [ ] Soporte para repositorios adicionales (Flatpak, Snap, Nix)
- [ ] Cacheado de descargas con política LRU
- [ ] Modo daemon para operaciones aún más rápidas
- [ ] Soporte para Windows (WSL2 ya funciona)
- [ ] Tests unitarios automatizados (Catch2 o GoogleTest)
- [ ] Configuración vía archivo `~/.gwins12/config.toml`
- [ ] Soporte para múltiples arquitecturas (arm64, i386) en el mismo pool
- [ ] Hacer todo el proyecto modulado 

### Cómo contribuir

1. Fork el repositorio
2. Crea una rama: `git checkout -b feature/nueva-funcionalidad`
3. Commitea tus cambios: `git commit -am 'Añade nueva funcionalidad'`
4. Push a la rama: `git push origin feature/nueva-funcionalidad`
5. Abre un Pull Request

---

## 🧪 Tests

```bash
# Tests automáticos del Makefile
make test

# Validaciones manuales realizadas
✅ Vector de prueba NIST SHA-256 (sha256("abc") = ba7816bf…15ad)
✅ Descarga real + verificación de hash correcto
✅ Rechazo de descarga con hash incorrecto (archivo eliminado)
✅ Búsqueda sobre 84.909 paquetes en ~100 ms
✅ Resolución SAT de dependencias (curl → zlib1g, libc6, …)
✅ Dry-run de clonado, bootstrap y todos los códigos de error
✅ Fallback HTTP sin git (GitHub/GitLab tarball)
✅ Auto-instalación de git vía gestor nativo
```

---

## 📄 Licencia

Proyecto educativo — úsalo libremente, modifícalo y mejóralo. Comparte tus cambios con la comunidad. 🚀

---

<div align="center">

**Hecho con ⚡ y C++20**

*¿Te gusta Gwins12? ¡Dale una ⭐ al repositorio!*

</div>
