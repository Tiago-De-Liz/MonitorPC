# MonitorPC

Widget minúsculo para Windows que fica sempre por cima de tudo, no canto do monitor, mostrando:

- **CPU**: uso em % e temperatura
- **GPU**: uso em % e temperatura
- **RAM**: uso em % e GB
- **FPS** do jogo aberto (só aparece quando tem jogo rodando)
- **Lembretes de manutenção** configuráveis (limpeza, pasta térmica, manutenção geral…), que só aparecem quando
  estão perto de vencer — aí o widget muda de cor e mostra os botões **Feito** e **Adiar**.

Feito para ficar ligado 24/7 sem pesar: Win32 puro em C++, ~250 KB, **~2 MB de RAM** e **~0,3% de um núcleo**.
Não usa GPU para desenhar (o desenho é em software, 1x por segundo, só quando algum valor muda).

## Tamanhos

| Modo | Como chegar |
|---|---|
| **Expandido** | padrão; clique no `+` da linha ou no ícone mini |
| **Linha** | `−` no canto do expandido |
| **Mini** (só um ícone) | `−` da linha |

Arraste para mudar de lugar (a posição fica salva). O canto mais próximo fica "ancorado": no canto inferior
direito, o widget cresce para cima e para a esquerda. Clique direito abre o menu (modo, opacidade, configurações,
iniciar com o Windows, sair).

## De onde vêm os números

| Dado | Fonte | Precisa de |
|---|---|---|
| CPU % | `GetSystemTimes` | — |
| RAM | `GlobalMemoryStatusEx` | — |
| GPU % e °C | D3DKMT (a mesma fonte do Gerenciador de Tarefas; qualquer fabricante) | — |
| CPU °C | registrador SMN dos AMD Ryzen, via driver [PawnIO](https://pawnio.eu) | administrador + PawnIO |
| FPS | eventos ETW de *Present* (DXGI, D3D9 e DxgKrnl — cobre DirectX, Vulkan e OpenGL), sem injetar nada no jogo | administrador |

O FPS mostrado é o do app **em tela cheia** que está em primeiro plano renderizando (dá para incluir apps em
janela nas Configurações). Navegadores, Discord etc. estão numa lista de ignorados (`fps_ignorar` no config).

> **Temperatura da CPU só em AMD Zen** (Ryzen/Threadripper). Em Intel o campo fica "–".
>
> **FACEIT bloqueia o driver PawnIO.** Se você joga FACEIT, não instale o PawnIO (o resto funciona sem ele).

## Instalar e compilar

Requisitos: Windows 10/11 x64 e as **Ferramentas de Build do Visual Studio** com "Desenvolvimento para desktop
com C++".

```bat
winget install --id namazso.PawnIO
build.bat run
```

Na primeira vez ele pede administrador (UAC) e registra a tarefa agendada **MonitorPC**, que abre o app já
elevado no logon, sem UAC a cada boot. Para tirar do início do Windows: menu → *Iniciar com o Windows*.

Linha de comando:

| Argumento | Efeito |
|---|---|
| `--quit` | fecha a instância aberta |
| `--no-elevate` | não pede administrador (sem FPS e sem temp. da CPU) |
| `--demo` | preenche FPS/temperatura com valores de exemplo, para conferir o layout |

## Arquivos

- Configuração: `%APPDATA%\MonitorPC\config.ini` (editável à mão com o app fechado)
- Log: `%APPDATA%\MonitorPC\monitorpc.log` (se algo não aparecer, o motivo está aqui)

## Código

| Arquivo | Conteúdo |
|---|---|
| `src/main.cpp` | janela, arrastar, modos, menus, sempre-no-topo |
| `src/render.cpp` | desenho em software (antialias próprio + ClearType) |
| `src/sensors.cpp` | CPU, RAM, GPU (D3DKMT) e temperatura via PawnIO |
| `src/fps.cpp` | contador de FPS por ETW |
| `src/tasks.cpp`, `src/config.cpp` | lembretes e config INI |
| `src/settings.cpp` | janela de Configurações |
| `src/autostart.cpp` | tarefa agendada de logon |
| `modules/` | módulo assinado do PawnIO (ver `modules/README.md`) |
