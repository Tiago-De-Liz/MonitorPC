# Módulos PawnIO

`AMDFamily17.bin` é o módulo oficial e assinado do [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules),
release **0.2.11** (`release_0_2_11.zip`, SHA-256 `43608cb89bc84247fef1368a139013f7d043e17db6d6c8dfc9b46bf0905a81f4`).

O MonitorPC usa só a função `ioctl_read_smn` dele para ler o registrador de temperatura dos Ryzen (Zen 1 a Zen 5).
O arquivo precisa ficar na mesma pasta do `MonitorPC.exe`; o `build.bat` copia ele para `build\`.

Licença do módulo: LGPL-2.1-or-later (© namazso). O código-fonte está em
[AMDFamily17.p](https://github.com/namazso/PawnIO.Modules/blob/main/AMDFamily17.p).
