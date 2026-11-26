<h1 align="center">🎥 Niriun para OBS Studio</h1>

<p align="center">
  <strong>O celular vira câmera do OBS, e o OBS avisa o celular quando ele está no ar.</strong><br>
  Plugin para OBS Studio que recebe vídeo e áudio do app <a href="https://github.com/leoberbert/Niriun">Niriun</a> pela rede Wi-Fi ou por cabo USB, sem programa extra no computador.
</p>

<p align="center">
  <a href="https://github.com/leoberbert/nirium-obs-plugin/releases/latest"><img src="https://img.shields.io/github/v/release/leoberbert/nirium-obs-plugin?style=for-the-badge&logo=github&color=blue" alt="Última versão"></a>
  <a href="https://github.com/leoberbert/nirium-obs-plugin/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/leoberbert/nirium-obs-plugin/ci.yml?style=for-the-badge&logo=githubactions&logoColor=white&label=CI" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/licen%C3%A7a-GPL--2.0--or--later-green?style=for-the-badge" alt="Licença GPL"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Windows-10%2F11-0078D6?style=flat-square&logo=windows&logoColor=white" alt="Windows">
  <img src="https://img.shields.io/badge/OBS%20Studio-32.x-302E31?style=flat-square&logo=obsstudio&logoColor=white" alt="OBS Studio">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white" alt="C++17">
  <img src="https://img.shields.io/github/stars/leoberbert/nirium-obs-plugin?style=flat-square&logo=github" alt="Stars">
  <img src="https://img.shields.io/github/issues/leoberbert/nirium-obs-plugin?style=flat-square" alt="Issues">
</p>

<p align="center">
  <a href="#-instalação">📥 Instalação</a> •
  <a href="#-primeiros-passos">🚀 Primeiros passos</a> •
  <a href="#-como-o-tally-funciona">🔴 Tally</a> •
  <a href="#-compilar">🔨 Compilar</a> •
  <a href="#-problemas-comuns">🛠️ Problemas</a> •
  <a href="README.en.md">🇺🇸 English</a>
</p>

---

## 🎯 Em resumo

| | |
|---|---|
| 📱 **Celular como câmera** | H.264 ou HEVC até 4K, com áudio Opus do microfone |
| 🔴 **Tally de verdade** | Borda **magenta** no celular quando a fonte está no ar e **âmbar** no preview |
| 🔌 **Wi-Fi ou USB** | Descoberta automática na rede, `adb` para cabo, ou IP manual |
| 🎛️ **Controles remotos** | Câmera, zoom, lanterna, espelho e travar foco, direto nas propriedades da fonte |
| 🪶 **Nada a mais no PC** | Sem cliente do Iriun, sem driver de câmera virtual, sem OBS WebSocket |
| 🆓 **Livre** | GPL-2.0-or-later |

```
celular (app Niriun) ──── Wi-Fi ou USB ────►  OBS + plugin Niriun
   câmera + microfone      H.264/HEVC + Opus     decodifica e mostra a fonte
   borda do tally     ◄──── tally (NO AR) ─────  sabe se a fonte está no programa
```

---

## 📥 Instalação

Baixe o arquivo da [página de releases](https://github.com/leoberbert/nirium-obs-plugin/releases/latest). Feche o OBS Studio antes de instalar.

### 🧩 Opção 1: instalador (recomendado)

1. Baixe `niriun-obs-<versão>-windows-x64-setup.exe`.
2. Execute (pede permissão de administrador). Ele acha sozinho a pasta do OBS (`C:\Program Files\obs-studio`) e avisa se não encontrar.
3. Avance até o fim e abra o OBS.

Para remover: **Configurações → Aplicativos → Niriun - plugin para OBS Studio → Desinstalar**.

### 📦 Opção 2: pacote portátil

1. Baixe `niriun-obs-<versão>-windows-x64-portable.zip` e descompacte.
2. **OBS 32:** copie as pastas `obs-plugins` e `data` para dentro da pasta do OBS, aceitando mesclar:

   | Arquivo | Destino |
   |---|---|
   | `obs-plugins\64bit\niriun-obs.dll` | `C:\Program Files\obs-studio\obs-plugins\64bit\` |
   | `data\obs-plugins\niriun-obs\locale\*.ini` | `C:\Program Files\obs-studio\data\obs-plugins\niriun-obs\locale\` |

   Em um OBS **portátil**, use a pasta onde ele foi descompactado no lugar de `C:\Program Files\obs-studio`.

   **OBS 33 ou mais novo** (a pasta do OBS tem `core\` em vez de `obs-plugins\`): copie a pasta `plugins` do zip para a pasta do OBS:

   | Arquivo | Destino |
   |---|---|
   | `plugins\niriun-obs\niriun-obs.dll` | `<pasta do OBS>\plugins\niriun-obs\` |
   | `plugins\niriun-obs\data\locale\*.ini` | `<pasta do OBS>\plugins\niriun-obs\data\locale\` |

   O instalador (.exe) escolhe sozinho o formato conforme a versão do OBS encontrada.

### 👤 Opção 3: só para o seu usuário (sem administrador)

Com o código-fonte baixado, no PowerShell:

```powershell
scripts\install-windows.ps1        # instala em %APPDATA%\obs-studio\plugins\niriun-obs
```

### ✅ Conferir se instalou

Abra o OBS, clique em **Fontes → +** e procure **Niriun** na lista. Se não aparecer, veja **Ajuda → Arquivos de log → Ver log atual** e procure por `niriun-obs`: a linha `module loaded` indica que o plugin carregou.

### 🐧 Linux

> ⚠️ **Estado:** o workflow de Release publica só o pacote do Windows. No Linux o plugin precisa ser **compilado** por você, e ele ainda **não foi executado** no Linux (só passou na checagem de sintaxe do `g++` e nos testes do protocolo). Se tentar, relate o resultado nas issues.

**1. Dependências para compilar** (cabeçalhos de desenvolvimento do OBS e do FFmpeg, versão compatível com o seu OBS 32.x):

```bash
# Debian/Ubuntu (OBS pelo PPA oficial ou pacote com libobs-dev)
sudo apt install build-essential cmake pkg-config git libobs-dev obs-studio libavcodec-dev libavutil-dev
# Fedora
sudo dnf install gcc-c++ cmake pkgconf-pkg-config git obs-studio-devel ffmpeg-free-devel
# Arch
sudo pacman -S base-devel cmake git obs-studio ffmpeg
```

**2. Compilar**

```bash
git clone https://github.com/leoberbert/nirium-obs-plugin.git && cd nirium-obs-plugin
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build                 # testes do protocolo (opcional)
```

**3. Instalar** (feche o OBS antes). Escolha uma das formas:

- **OBS do sistema (apt, dnf, pacman), todos os usuários:**

  ```bash
  sudo cmake --install build --prefix /usr
  ```

  Isso coloca `niriun-obs.so` em `/usr/lib/obs-plugins/` (em algumas distros a pasta é `/usr/lib64/obs-plugins/` ou `/usr/lib/x86_64-linux-gnu/obs-plugins/`; se for o seu caso, copie o `.so` para lá) e os idiomas em `/usr/share/obs/obs-plugins/niriun-obs/locale/`.

- **Só para o seu usuário (sem sudo)**, funciona com o OBS do sistema, AppImage ou OBS compilado:

  ```bash
  P=~/.config/obs-studio/plugins/niriun-obs
  mkdir -p "$P/bin/64bit" "$P/data/locale"
  cp build/niriun-obs.so "$P/bin/64bit/"
  cp data/locale/*.ini "$P/data/locale/"
  ```

- **OBS portátil** (pasta descompactada, tarball ou AppImage extraído com `--appimage-extract`): copie para dentro da pasta do OBS, mantendo a mesma estrutura do plugin do usuário:

  ```bash
  P=/caminho/do/obs/plugins/niriun-obs      # ou o diretório de plugins do seu build do OBS
  mkdir -p "$P/bin/64bit" "$P/data/locale"
  cp build/niriun-obs.so "$P/bin/64bit/"
  cp data/locale/*.ini "$P/data/locale/"
  ```

  Se o seu OBS compilado usa `--prefix`, use `cmake --install build --prefix <esse prefixo>`.

- **Flatpak (`com.obsproject.Studio`):** o OBS roda isolado e usa o FFmpeg do runtime Flatpak, então o `.so` compilado contra o FFmpeg do sistema **provavelmente não carrega**. O plugin deve ser compilado contra o mesmo `libobs` e FFmpeg do Flatpak (SDK `org.freedesktop.Sdk` com a extensão do OBS). Com o `.so` compatível em mãos, instale por usuário:

  ```bash
  P=~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/niriun-obs
  mkdir -p "$P/bin/64bit" "$P/data/locale"
  cp niriun-obs.so "$P/bin/64bit/"
  cp data/locale/*.ini "$P/data/locale/"
  ```

  Limitações dentro do sandbox: o Wi-Fi funciona, mas o **`adb` (USB) não está disponível** no Flatpak por padrão; use Wi-Fi ou IP manual. [INFERÊNCIA, não testado] Como alternativa, rode `adb forward tcp:4699 tcp:4699` no sistema e escolha no OBS "Usar IP do WiFi" com `127.0.0.1`.

**4. Rede:** o OBS precisa receber o beacon UDP **4698** (multicast) e abrir TCP **4699** até o celular (a descoberta por varredura usa a TCP **4697**). Com firewall ativo (`ufw`, `firewalld`) libere a UDP 4698 de entrada; se o celular não aparecer sozinho, use o IP manual.

Depois de instalar, confira como em [Conferir se instalou](#-conferir-se-instalou). No Linux o log fica em `~/.config/obs-studio/logs/` (Flatpak: `~/.var/app/com.obsproject.Studio/config/obs-studio/logs/`).

Para remover: apague os arquivos que você copiou (`niriun-obs.so` e a pasta `niriun-obs` de dados) ou a pasta `~/.config/obs-studio/plugins/niriun-obs`.

### 📋 Requisitos

- 🪟 Windows 10 ou 11 (64 bits), ou 🐧 Linux 64 bits (compilado por você, veja [Linux](#-linux))
- 🎬 OBS Studio **32.x ou 33.x** (testado: 32.2.2 e 33.0.3): o plugin usa o FFmpeg do próprio OBS e se recusa a carregar se a versão for incompatível (precisa do mesmo `libavcodec`, hoje `avcodec-62`). Em outra versão do OBS, [compile o plugin](#-compilar) contra ela.
- 📱 Celular Android com o app [Niriun](https://github.com/leoberbert/Niriun) com suporte ao tally (a versão seguinte à 0.1.2; o vídeo e o áudio funcionam com a 0.1.1 e a 0.1.2, sem tally)
- 🔌 *Opcional (USB):* `adb` no PATH ou o Android SDK instalado

---

## 🚀 Primeiros passos

1. 📱 **No celular:** abra o Niriun e toque em **Iniciar**.
2. 🎬 **No OBS:** **Fontes → + → Niriun** e dê um nome.
3. 🔎 Em **Dispositivo**, escolha o celular:
   - ele aparece sozinho se estiver na mesma rede Wi-Fi (`Nome [WiFi] (192.168.x.x)`);
   - por cabo, escolha a entrada `[USB]` (ative a depuração USB no celular);
   - se não aparecer, clique em **Atualizar lista**, ou escolha **Usar IP do WiFi** e digite o endereço mostrado em *Detalhes* no app.
4. ▶️ Clique em **Ativar**. A imagem aparece em alguns segundos.
5. 🔴 Coloque a fonte em uma cena e acompanhe a borda do celular mudar.

### ⚙️ Propriedades da fonte

| Opção | Para que serve |
|---|---|
| **Resolução / Taxa de quadros** | O que o celular codifica (de 640×360 a 3840×2160, 24/30/60 fps) |
| **Formato de vídeo** | H.264 (padrão) ou HEVC |
| **Matriz de cor** | Auto, Rec. 601 ou Rec. 709, se as cores parecerem erradas |
| **Ativar áudio** | Recebe o microfone do celular |
| **Enviar tally ao celular** | Liga ou desliga a borda NO AR / preview |
| **Desconectar quando a fonte não estiver visível** | Economiza bateria do celular |
| **Latência ultrabaixa** | Sem buffer no OBS (padrão); desligue para saída mais suave |
| **Aceleração por hardware** | Decodifica na GPU quando possível |
| **Controles da câmera** | Câmera, zoom, lanterna, espelho, travar exposição e foco |

Qualquer mudança que afete o vídeo reconecta sozinha. Se o celular sumir, o plugin continua tentando.

---

## 🔴 Como o tally funciona

O OBS já sabe em que estado cada fonte está, e o plugin repassa isso ao celular pela mesma conexão do vídeo:

| No OBS | No celular |
|---|---|
| Fonte **no programa** (saída) | 🟣 Borda magenta e `AO VIVO · nome da fonte` |
| Fonte **só no preview** (Modo Estúdio) | 🟠 Borda âmbar e `PREVIEW · nome da fonte` |
| Fonte fora de uso | Sem borda |

Com a fonte no ar, o app pede **dupla confirmação** antes de parar a transmissão.

> ℹ️ Abrir o *Multiview* ou um projetor de uma cena também conta como "visível" e acende o preview.

---

## 🔍 Como o celular é encontrado

| Mecanismo | O que precisa |
|---|---|
| 📡 Beacon UDP multicast (`239.255.188.58:4698`), enviado pelo celular a cada 2 s | Na primeira vez o Windows pede permissão no firewall para o OBS |
| 🧭 **Atualizar lista** varre a rede /24 na porta TCP 4697 | Só conexões de saída: funciona onde o multicast é bloqueado |
| 🔌 USB: `adb forward` | `adb` no PATH ou no Android SDK |
| ✍️ IP manual | O endereço do celular, porta 4699 |

---

## 🔨 Compilar

Windows com Visual Studio 2022, CMake 3.20+ e git:

```powershell
scripts\fetch-windows-deps.ps1                  # cabeçalhos e bibliotecas do OBS instalado (-ObsDir para outra pasta)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNIRIUN_DEPS_DIR="$PWD\deps"
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo        # testes do protocolo
scripts\install-windows.ps1                     # instala para o seu usuário
```

Linux e macOS: `cmake -S . -B build` com `libobs`, `obs-frontend-api` e FFmpeg disponíveis. No Linux o código passa na checagem de sintaxe do `g++` e nos testes unitários, mas o plugin só foi executado no Windows. O macOS não foi compilado.

### 🤖 Gerar o instalador (GitHub Actions)

O workflow [`release.yml`](.github/workflows/release.yml) é **manual** e publica uma GitHub Release com o instalador (`.exe`, Inno Setup), o pacote portátil (`.zip`) e o `SHA256SUMS.txt`:

1. Ajuste a versão em `CMakeLists.txt` (`project(niriun-obs VERSION x.y.z ...)`) e faça merge na `master`.
2. **Actions → Release → Run workflow**, informando a tag `vx.y.z` (e marcando *prerelease*, se for o caso).

O workflow confere a tag, roda o CI, compila e publica. Detalhes em [`docs/ci-cd.md`](docs/ci-cd.md). O [`ci.yml`](.github/workflows/ci.yml) compila e testa a cada push na `master` e em pull requests.

Para gerar o instalador na sua máquina: instale o [Inno Setup 6](https://jrsoftware.org/isinfo.php), rode `cmake --install build --config RelWithDebInfo --prefix dist` e depois `iscc /DAppVersion=0.1.4 installer\niriun-obs.iss`. O resultado fica em `dist-installer\`.

---

## 🧪 Testes

| O que | Como |
|---|---|
| Protocolo (sem OBS) | `ctest` |
| Celular simulado | `node tools/phone-sim/phone-sim.js` e uma fonte com *IP do WiFi* `127.0.0.1` (precisa do `ffmpeg`) |
| Ponta a ponta com OBS | `node tools/e2e/e2e.js --obs <pasta de um OBS portátil>`: descoberta, vídeo, áudio, tally, controles, rotação, HEVC e reconexão |
| Com o app de verdade | `node tools/e2e/app-e2e.js --obs <pasta> --serial <serial do adb>` |

---

## 📡 Protocolo

O protocolo está documentado no repositório do app (`docs/PROTOCOL.md`). O plugin é o lado do computador:

| Sentido | Mensagem | Quando |
|---|---|---|
| celular → PC | `0x01` hello, `0x0A` tamanhos, `0x18` câmeras | ao conectar |
| PC → celular | `0x0B` START (áudio, codec, resolução, fps) | depois dos tamanhos |
| celular → PC | `0x08` vídeo, `0x07` áudio Opus | contínuo |
| PC → celular | `0x09` ACK de cada pacote de vídeo | a cada decodificação |
| PC → celular | `0x10` controles da câmera | pelas propriedades da fonte |
| PC → celular | **`0x30` tally** (estado 0/1/2 e nome da fonte) | depois do START e a cada mudança |
| PC → celular | `0x01` vazio | a cada 3 s parado (o celular derruba cliente mudo após 10 s) |
| celular → PC | **`0x31` tela ao parar** (modo 1 preto, 2 cartão offline) | uma vez, ao tocar em Parar |

O `0x30` e o `0x31` são extensões do Niriun; o app original do Iriun as ignora. Com o `0x31` o app diz ao plugin o que deixar na fonte depois de um Parar deliberado (quadro preto ou um cartão offline); se a conexão cair, a fonte fica sem imagem como antes.

---

## 🛠️ Problemas comuns

| Sintoma | O que fazer |
|---|---|
| **Niriun** não aparece em *Fontes* | Confirme que o OBS é 64 bits, versão 32.x, e procure `niriun-obs` no log do OBS |
| Log diz `libavcodec major version ... does not match` | O OBS usa outro FFmpeg: compile o plugin contra a sua versão do OBS |
| O celular não aparece na lista | Mesma rede Wi-Fi? Clique em **Atualizar lista**; permita o OBS no firewall (redes privadas); ou use **IP do WiFi** |
| Fica em "Procurando o celular..." | O app precisa estar em **Iniciar**; outro programa (ou outro OBS) pode estar conectado ao celular, que aceita um cliente por vez |
| Entrada `[USB]` não aparece | Ative a depuração USB, aceite a chave no celular e confira `adb devices` |
| Cores diferentes da câmera | Troque a **Matriz de cor** para Rec. 601 ou Rec. 709 |
| HEVC reconecta sem parar | O codificador do celular recusou o HEVC: use H.264 |
| O tally não acende no celular | Use uma versão do app com suporte ao tally (posterior à 0.1.2) e confira **Enviar tally ao celular** |

---

## 🙏 Origem e licença

O código partiu do [droidcam-obs-plugin](https://github.com/dev47apps/droidcam-obs-plugin) (GPL-2.0-or-later), da Dev47Apps, versão 2.5.1 (commit `ddfa888`). Este repositório é **independente**: não é um fork do GitHub e não acompanha o original, mas o histórico ainda contém os commits dele. O protocolo do DroidCam foi trocado pelo do Niriun, e foram removidos o MJPEG, o iOS, o navegador mDNS e a janela Qt.

Licença **GPL-2.0-or-later**, como o original (veja [LICENSE](LICENSE)). Partes © DEV47APPS e © Hugh Bailey (decodificação FFmpeg do OBS Studio); código novo © Leonardo Berbert. O app Niriun é outro programa (Apache-2.0) que conversa com o plugin pela rede.
