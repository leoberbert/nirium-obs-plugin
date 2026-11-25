# CI/CD - GitHub Actions

## Workflows

### CI (`.github/workflows/ci.yml`)
Roda em push na `master`, em pull request e manualmente. Também é chamado pelo Release.

| Job | O que faz |
| --- | --- |
| `Testes do protocolo` | Compila e roda `ctest` no Ubuntu (não precisa do OBS) |
| `Plugin Windows x64` | Baixa o OBS Studio, gera cabeçalhos e bibliotecas de importação, compila o plugin, roda os testes e publica a pasta `dist` como artefato (7 dias) |

### Release (`.github/workflows/release.yml`)
Somente manual (Actions -> Release -> Run workflow).

| Input | Descrição |
| --- | --- |
| `tag` | Ex.: `v0.1.0`. Precisa ser igual à versão em `CMakeLists.txt` (`project(niriun-obs VERSION ...)`); o workflow confere |
| `prerelease` | Marca a release como pré-release |

O fluxo é: validar a tag (formato, igual à versão do CMake, ainda inexistente, branch padrão) -> rodar o CI -> compilar -> gerar os arquivos -> criar a release.

Arquivos publicados:

| Arquivo | Conteúdo |
| --- | --- |
| `niriun-obs-<versão>-windows-x64-setup.exe` | Instalador (Inno Setup, `installer/niriun-obs.iss`) |
| `niriun-obs-<versão>-windows-x64-portable.zip` | `obs-plugins` e `data` para copiar na pasta do OBS |
| `SHA256SUMS.txt` | Somas SHA-256 dos arquivos acima |

Não há secrets: o workflow usa só o `GITHUB_TOKEN` (permissão `contents: write`). Os binários não são assinados, então o Windows SmartScreen pode avisar na primeira execução do instalador.

## Publicar uma versão
1. Ajuste a versão em `CMakeLists.txt` (`project(niriun-obs VERSION x.y.z ...)`) e faça merge na `master`.
2. Actions -> Release -> Run workflow, com a tag `vx.y.z`.
3. Confira a release e os arquivos na página de releases.

## Se o OBS mudar de versão
A versão do OBS usada na compilação está em `OBS_VERSION` (`ci.yml` e `release.yml`). O plugin só carrega em um OBS cujo FFmpeg tenha a mesma versão maior; ao subir o `OBS_VERSION`, gere uma nova release.

## Build local
Veja o README (seções Compilar e Gerar o instalador).
