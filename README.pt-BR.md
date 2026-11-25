# Plugin Niriun para o OBS

Uma fonte do OBS Studio que recebe a câmera e o microfone de um celular com o [Niriun](https://github.com/leoberbert/Niriun)
e avisa o celular quando a fonte está **no ar** ou em **preview** (tally).

Partiu do [droidcam-obs-plugin](https://github.com/dev47apps/droidcam-obs-plugin) (GPL-2.0-or-later), da Dev47Apps, versão 2.5.1
(commit `ddfa888`). Este repositório é independente (não é um fork do GitHub) e não acompanha o original; o histórico ainda
contém os commits dele. O protocolo do DroidCam foi trocado pelo do Niriun. O plugin é o lado do computador do protocolo do Niriun, então **não é preciso
instalar mais nada no computador**: nem o cliente do Iriun Webcam, nem driver de câmera virtual, nem o OBS WebSocket.

## Como usar

1. Instale o plugin (veja o [README](README.md#install-windows-obs-studio-32x)) e reinicie o OBS.
2. No celular: abra o Niriun e toque em **Iniciar**.
3. No OBS: **Fontes → + → Niriun**. Escolha o celular em **Dispositivo** (ele aparece sozinho na mesma rede Wi-Fi; por cabo
   use a entrada `[USB]`; ou use *IP do WiFi* e digite o endereço mostrado em Detalhes no app) e clique em **Ativar**.
4. A borda do celular fica **magenta** quando a fonte está na saída (programa) e **âmbar** no preview (Modo Estúdio). O app
   pede dupla confirmação antes de parar enquanto você está no ar.

Propriedades: resolução, taxa de quadros, H.264/HEVC, áudio, matriz de cor, decodificação por hardware, baixa latência,
"desconectar quando a fonte não estiver visível" e controles remotos da câmera (câmera, zoom, lanterna, espelho, travar
exposição/foco). Qualquer mudança que afete o vídeo reconecta sozinha; se o celular sumir, o plugin continua tentando.

## Como o celular é encontrado

* Beacon UDP multicast `239.255.188.58:4698` (o celular envia a cada 2 s enquanto não há computador conectado). O Windows
  pede permissão no firewall na primeira vez.
* **Atualizar lista** varre a sub-rede /24 na porta TCP 4697 (só conexões de saída; funciona onde o multicast é bloqueado).
* USB: `adb forward`. O `adb` é procurado no PATH e no Android SDK. É opcional.
* *IP do WiFi*: endereço do celular, porta 4699.

## Compilar e testar

```powershell
scripts\fetch-windows-deps.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNIRIUN_DEPS_DIR="$PWD\deps"
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo
scripts\install-windows.ps1
```

`tools/phone-sim/phone-sim.js` simula o celular (H.264/HEVC e Opus reais via ffmpeg). `tools/e2e/e2e.js` abre um OBS portátil
com o plugin e confere descoberta, vídeo, áudio, tally, controles, rotação, reconexão e HEVC; `tools/e2e/app-e2e.js` faz o
vídeo e o tally contra o app Android de verdade por `adb`.

## Licença

GPL-2.0-or-later, como o original. Veja [LICENSE](LICENSE). O app Niriun é outro programa (Apache-2.0) que conversa com o
plugin pela rede.
