# Forensic Tool Downloader

Aplicacao Win32 em C++ para baixar ferramentas em pastas separadas por autor/usuario.

## Build no Linux com MinGW

```bash
cmake -S . -B build -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres
cmake --build build -j
```

O executavel gerado fica em `build/ForensicToolDownloader.exe`. A pasta `dist` contem o executavel e o exemplo de `scripts.ini` prontos para copiar.

## Uso

Execute o `.exe` no Windows. As pastas `Others`, `Spokwn`, `Orbdiff` e `Nirsoft` sao criadas no mesmo diretorio do executavel.

O botao `Download All Tools` baixa todas as ferramentas. Em cada aba de download, o botao `Baixar esta aba` no canto inferior direito baixa somente as ferramentas daquela aba.

Os downloads em lote rodam em paralelo, com ate 4 downloads simultaneos.

Arquivos `.zip` seguem as caixas `Perguntar extrair ZIP` e `Perguntar apagar ZIP`: quando marcadas, o app pergunta ao usuario; quando desmarcadas, o app extrai automaticamente e apaga o compactado depois da extracao.

A caixa `Modo quadrados` alterna as abas de download entre lista e cards compactos.

## Scripts

Os scripts `Habilitar servicos`, `Kernel Scanner` e `Eventvwr Logs Scanner` ja vem embutidos. Os dois scanners do Nacio salvam resultados em `Desktop\ForensicToolReports`.

Para adicionar mais botoes na aba `Scripts`, copie `scripts.example.ini` para `scripts.ini` ao lado do executavel e edite as secoes. Use `Elevated=1` quando quiser abrir o script como administrador.

## Atalhos Windows

A aba `Atalhos Windows` abre locais e paineis do Windows como Prefetch, Temp, Recent, Event Viewer, Services, Programas instalados e PSReadLine. Cada item mostra o comando equivalente do Windows+R no tooltip e possui botao `Copiar`.