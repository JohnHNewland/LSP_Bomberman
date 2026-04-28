# LSP_Bomberman

Šis projekts ir retro spēles "Bomberman" klienta un servera izveide, izmantojot C99 un sekojot [LSP_game_specs_2026 (Google Docs).](https://docs.google.com/document/d/15ppHdVY86KLds5sUN0xMjUNuTLPjXw9DdnnMYsGTgok/edit?usp=sharing)

### Veidoja
- Jānis Helvijs Jaunzems, jj24017
- Roberts Leizāns, rl24021

### Ieguldījums
- Jānis Helvijs Jaunzems: 50%
- Roberts Leizāns: 50%

### Spēles servera palaišana:
- Nepieciešamie faili:
- - `maps` direktorija un tās saturs (spēles kartes)
- - `common` direktorija un tās saturs (koplietotās funkcijas)
- - `server` direktorija un tās saturs (servera programmas kods)
- - `makefile` kompilēšanas un palaišanas komandas
- Palaišana:
- - `make create_server` (kompilēšana)
- - `make start_server` (palaišana)

### Spēles klienta palaišana:
- Nepieciešamie faili:
- - `common` direktorija un tās saturs (koplietotās funkcijas)
- - `client` direktorija un tās saturs (klienta programmas kods)
- - `makefile` kompilēšanas un palaišanas komandas
- Palaišana:
- - `make create_client` (kompilēšana)
- - `make start_server` (palaišana)