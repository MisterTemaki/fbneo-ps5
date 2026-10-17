// FBNeo PS5 frontend: the interface's languages (fe_i18n.h).
// SPDX-License-Identifier: MIT

#include "fe_i18n.h"

#include "fe_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace fe
{
namespace
{
// English, Spanish, Portuguese (Brazil), French, German. A text missing here is shown in English.
struct Entry
{
	const char* en;
	const char* es;
	const char* pt;
	const char* fr;
	const char* de;
};

const Entry kTable[] = {
	// ---- the settings
	{"On", "Sí", "Sim", "Oui", "Ein"},
	{"Off", "No", "Não", "Non", "Aus"},
	{"VIDEO", "VÍDEO", "VÍDEO", "VIDÉO", "VIDEO"},
	{"Shader", "Shader", "Shader", "Shader", "Shader"},
	{"Screen size", "Tamaño de la imagen", "Tamanho da imagem", "Taille de l'image", "Bildgröße"},
	{"Aspect ratio", "Relación de aspecto", "Proporção da tela", "Format d'image", "Seitenverhältnis"},
	{"Smooth picture", "Imagen suavizada", "Imagem suavizada", "Image lissée", "Geglättetes Bild"},
	{"(shader)", "(shader)", "(shader)", "(shader)", "(Shader)"},
	{"Scanlines (CRT effect)", "Scanlines (efecto CRT)", "Scanlines (efeito CRT)", "Lignes de balayage (effet CRT)",
		"Scanlines (Röhreneffekt)"},
	{"Show FPS", "Mostrar FPS", "Mostrar FPS", "Afficher les FPS", "FPS anzeigen"},
	{"AUDIO", "AUDIO", "ÁUDIO", "AUDIO", "AUDIO"},
	{"Sound", "Sonido", "Som", "Son", "Ton"},
	{"Volume", "Volumen", "Volume", "Volume", "Lautstärke"},
	{"EMULATION", "EMULACIÓN", "EMULAÇÃO", "ÉMULATION", "EMULATION"},
	{"Save high scores (hiscore.dat)", "Guardar récords (hiscore.dat)", "Salvar recordes (hiscore.dat)",
		"Sauver les meilleurs scores (hiscore.dat)", "Highscores speichern (hiscore.dat)"},
	{"CONTROLS (arcade button  ->  PS5 button)", "CONTROLES (botón arcade  ->  botón PS5)",
		"CONTROLES (botão do arcade  ->  botão do PS5)", "COMMANDES (bouton arcade  ->  bouton PS5)",
		"STEUERUNG (Arcade-Taste  ->  PS5-Taste)"},
	{"Button layout", "Distribución de botones", "Layout dos botões", "Disposition des boutons", "Tastenbelegung"},
	{"Default button layout", "Botones por defecto", "Botões padrão", "Boutons par défaut", "Standardbelegung"},
	{"LIBRARY", "BIBLIOTECA", "BIBLIOTECA", "BIBLIOTHÈQUE", "BIBLIOTHEK"},
	{"Download covers", "Descargar portadas", "Baixar capas", "Télécharger les jaquettes", "Cover herunterladen"},
	{"Show clones (other versions)", "Mostrar clones (otras versiones)", "Mostrar clones (outras versões)",
		"Afficher les clones (autres versions)", "Klone zeigen (andere Versionen)"},
	{"Show incomplete sets", "Mostrar sets incompletos", "Mostrar sets incompletos", "Afficher les sets incomplets",
		"Unvollständige Sets zeigen"},
	{"BIOS sets (Cross: details)", "Sets de BIOS (Cruz: detalles)", "Sets de BIOS (X: detalhes)",
		"Sets de BIOS (Croix : détails)", "BIOS-Sets (Kreuz: Details)"},
	{"None needed", "Ninguno necesario", "Nenhum necessário", "Aucun requis", "Keins nötig"},
	{"All OK", "Todo bien", "Tudo OK", "Tout est OK", "Alles OK"},
	{"%d missing", "Faltan %d", "Faltam %d", "%d manquants", "%d fehlen"},
	{"SYSTEM", "SISTEMA", "SISTEMA", "SYSTÈME", "SYSTEM"},
	{"Language", "Idioma", "Idioma", "Langue", "Sprache"},
	{"Debug logs", "Registros de depuración", "Registros de depuração", "Journaux de débogage", "Debug-Protokolle"},
	{"Updates (this is %s)", "Actualizaciones (esta es la %s)", "Atualizações (esta é a %s)",
		"Mises à jour (version actuelle %s)", "Updates (diese ist %s)"},
	{"Needs the helper", "Necesita el helper", "Precisa do helper", "Nécessite le helper", "Braucht den Helper"},
	{"Checking...", "Comprobando...", "Verificando...", "Vérification...", "Prüfe..."},
	{"Up to date", "Al día", "Atualizado", "À jour", "Aktuell"},
	{"Install %s (%.1f MB)", "Instalar %s (%.1f MB)", "Instalar %s (%.1f MB)", "Installer %s (%.1f Mo)",
		"%s installieren (%.1f MB)"},
	{"Downloading %d%%", "Descargando %d%%", "Baixando %d%%", "Téléchargement %d%%", "Lade herunter %d%%"},
	{"Installing %d/%d", "Instalando %d/%d", "Instalando %d/%d", "Installation %d/%d", "Installiere %d/%d"},
	{"Installed: restart", "Instalada: reinicia", "Instalada: reinicie", "Installée : redémarrez",
		"Installiert: neu starten"},
	{"Failed: try again", "Falló: inténtalo de nuevo", "Falhou: tente de novo", "Échec : réessayez",
		"Fehlgeschlagen: erneut versuchen"},
	{"Check now", "Comprobar ahora", "Verificar agora", "Vérifier maintenant", "Jetzt prüfen"},
	{"Updates are downloaded by the helper: send the payload, then open Settings again.",
		"El helper descarga las actualizaciones: envía el payload y vuelve a abrir Ajustes.",
		"O helper baixa as atualizações: envie o payload e abra as Configurações de novo.",
		"Le helper télécharge les mises à jour : envoyez le payload, puis rouvrez les Paramètres.",
		"Updates lädt der Helper: Payload senden, dann die Einstellungen erneut öffnen."},
	{"Back", "Volver", "Voltar", "Retour", "Zurück"},
	{"Settings", "Ajustes", "Configurações", "Paramètres", "Einstellungen"},
	{"Change", "Cambiar", "Mudar", "Changer", "Ändern"},
	{"Adjust", "Ajustar", "Ajustar", "Régler", "Anpassen"},
	// ---- the settings' choices
	{"Fit to screen", "Ajustar a la pantalla", "Ajustar à tela", "Adapter à l'écran", "An Bildschirm anpassen"},
	{"Integer scale", "Escala entera", "Escala inteira", "Échelle entière", "Ganzzahlige Skalierung"},
	{"Stretch to 16:9", "Estirar a 16:9", "Esticar para 16:9", "Étirer en 16:9", "Auf 16:9 strecken"},
	{"The game's monitor", "El monitor del juego", "O monitor do jogo", "Le moniteur du jeu", "Monitor des Spiels"},
	{"Square pixels", "Píxeles cuadrados", "Pixels quadrados", "Pixels carrés", "Quadratische Pixel"},
	{"16:9 (stretched)", "16:9 (estirado)", "16:9 (esticado)", "16:9 (étiré)", "16:9 (gestreckt)"},
	{"Auto", "Auto", "Auto", "Auto", "Auto"},
	{"Classic", "Clásica", "Clássico", "Classique", "Klassisch"},
	{"Fighting (6 buttons)", "Lucha (6 botones)", "Luta (6 botões)", "Combat (6 boutons)", "Kampf (6 Tasten)"},
	{"Custom", "Personalizada", "Personalizado", "Personnalisée", "Eigene"},
	{"Fighting (R1 R2)", "Lucha (R1 R2)", "Luta (R1 R2)", "Combat (R1 R2)", "Kampf (R1 R2)"},
	{"Cross", "Cruz", "X", "Croix", "Kreuz"},
	{"Circle", "Círculo", "Bola", "Rond", "Kreis"},
	{"Square", "Cuadrado", "Quadrado", "Carré", "Quadrat"},
	{"Triangle", "Triángulo", "Triângulo", "Triangle", "Dreieck"},
	{"Touchpad", "Panel táctil", "Touchpad", "Pavé tactile", "Touchpad"},
	{"None", "Ninguno", "Nenhum", "Aucun", "Keins"},
	{"Button 1", "Botón 1", "Botão 1", "Bouton 1", "Taste 1"},
	{"Button 2", "Botón 2", "Botão 2", "Bouton 2", "Taste 2"},
	{"Button 3", "Botón 3", "Botão 3", "Bouton 3", "Taste 3"},
	{"Button 4", "Botón 4", "Botão 4", "Bouton 4", "Taste 4"},
	{"Button 5", "Botón 5", "Botão 5", "Bouton 5", "Taste 5"},
	{"Button 6", "Botón 6", "Botão 6", "Bouton 6", "Taste 6"},
	{"Coin", "Moneda", "Ficha", "Pièce", "Münze"},
	{"Start", "Start", "Start", "Start", "Start"},
	// ---- Settings, BIOS sets
	{"BIOS sets", "Sets de BIOS", "Sets de BIOS", "Sets de BIOS", "BIOS-Sets"},
	{"NEEDED BY YOUR GAMES", "NECESARIOS PARA TUS JUEGOS", "NECESSÁRIOS PARA SEUS JOGOS", "REQUIS PAR VOS JEUX",
		"VON DEINEN SPIELEN BENÖTIGT"},
	{"OTHER BIOS SETS (none of your games needs them)", "OTROS SETS DE BIOS (ninguno de tus juegos los necesita)",
		"OUTROS SETS DE BIOS (nenhum dos seus jogos precisa deles)",
		"AUTRES SETS DE BIOS (aucun de vos jeux n'en a besoin)", "ANDERE BIOS-SETS (keines deiner Spiele braucht sie)"},
	{"Missing", "Falta", "Faltando", "Manquant", "Fehlt"},
	{"Not found", "No encontrado", "Não encontrado", "Introuvable", "Nicht gefunden"},
	{"%zu ROM missing", "Falta %zu ROM", "Falta %zu ROM", "%zu ROM manquante", "%zu ROM fehlt"},
	{"%zu ROMs missing", "Faltan %zu ROMs", "Faltam %zu ROMs", "%zu ROM manquantes", "%zu ROMs fehlen"},
	{"Needed by %d of your games (%d in FBNeo).", "Lo necesitan %d de tus juegos (%d en FBNeo).",
		"Necessário para %d dos seus jogos (%d no FBNeo).", "Requis par %d de vos jeux (%d dans FBNeo).",
		"Von %d deiner Spiele benötigt (%d in FBNeo)."},
	{"Needed by %d of FBNeo's games, none of them in your library.",
		"Lo necesitan %d juegos de FBNeo, ninguno en tu biblioteca.",
		"Necessário para %d jogos do FBNeo, nenhum na sua biblioteca.",
		"Requis par %d jeux de FBNeo, aucun dans votre bibliothèque.",
		"Von %d FBNeo-Spielen benötigt, keines davon in deiner Bibliothek."},
	{"Not found: put %s.zip (FBNeo %s's set) in /data/fbneo/roms or fbneo/roms on a USB drive.",
		"No encontrado: pon %s.zip (el set de FBNeo %s) en /data/fbneo/roms o fbneo/roms en un USB.",
		"Não encontrado: coloque %s.zip (o set do FBNeo %s) em /data/fbneo/roms ou fbneo/roms num pendrive.",
		"Introuvable : mettez %s.zip (le set de FBNeo %s) dans /data/fbneo/roms ou fbneo/roms sur une clé USB.",
		"Nicht gefunden: %s.zip (das Set von FBNeo %s) nach /data/fbneo/roms oder fbneo/roms auf einem USB-Stick legen."},
	{"%s and %zu more", "%s y %zu más", "%s e mais %zu", "%s et %zu de plus", "%s und %zu weitere"},
	{"%s lacks: %s.", "En %s faltan: %s.", "Em %s faltam: %s.", "Il manque dans %s : %s.", "In %s fehlen: %s."},
	{"Found: %s.", "Encontrado: %s.", "Encontrado: %s.", "Trouvé : %s.", "Gefunden: %s."},
	{"Optional versions found: %d of %d (the BIOS DIP switch picks one).",
		"Versiones opcionales encontradas: %d de %d (el DIP switch de la BIOS elige una).",
		"Versões opcionais encontradas: %d de %d (o DIP switch da BIOS escolhe uma).",
		"Versions optionnelles trouvées : %d sur %d (le DIP switch de la BIOS en choisit une).",
		"Optionale Versionen gefunden: %d von %d (der BIOS-DIP-Schalter wählt eine)."},
	{"Optional versions found: %d of %d.", "Versiones opcionales encontradas: %d de %d.",
		"Versões opcionais encontradas: %d de %d.", "Versions optionnelles trouvées : %d sur %d.",
		"Optionale Versionen gefunden: %d von %d."},
	{"No game in FBNeo's list needs a BIOS set.", "Ningún juego de la lista de FBNeo necesita un set de BIOS.",
		"Nenhum jogo da lista do FBNeo precisa de um set de BIOS.", "Aucun jeu de la liste de FBNeo n'a besoin d'un set de BIOS.",
		"Kein Spiel in FBNeos Liste braucht ein BIOS-Set."},
	{"The list is from the last time the game list was read (the shelf reads it each time it opens).",
		"La lista es de la última vez que se leyó la lista de juegos (el estante la lee cada vez que se abre).",
		"A lista é da última vez que a lista de jogos foi lida (a estante a lê sempre que abre).",
		"La liste date de la dernière lecture de la liste des jeux (l'étagère la relit à chaque ouverture).",
		"Die Liste stammt vom letzten Einlesen der Spieleliste (das Regal liest sie bei jedem Öffnen)."},
	// ---- the pause menu
	{"Resume", "Continuar", "Continuar", "Reprendre", "Fortsetzen"},
	{"Save state (slot %d)", "Guardar estado (ranura %d)", "Salvar estado (slot %d)", "Sauvegarder l'état (emplacement %d)",
		"Spielstand speichern (Platz %d)"},
	{"Load state (slot %d)", "Cargar estado (ranura %d)", "Carregar estado (slot %d)", "Charger l'état (emplacement %d)",
		"Spielstand laden (Platz %d)"},
	{"State slot", "Ranura de estado", "Slot do estado", "Emplacement", "Spielstand-Platz"},
	{"%d (used)", "%d (usada)", "%d (usado)", "%d (utilisé)", "%d (belegt)"},
	{"%d (empty)", "%d (vacía)", "%d (vazio)", "%d (vide)", "%d (leer)"},
	{"DIP switches", "DIP switches", "DIP switches", "DIP switches", "DIP-Schalter"},
	{"DIP switches  (most take effect after Reset)", "DIP switches  (la mayoría se aplican tras Reiniciar)",
		"DIP switches  (a maioria vale após Reiniciar)", "DIP switches  (la plupart après Réinitialiser)",
		"DIP-Schalter  (meist erst nach Reset)"},
	{"Default DIP switches", "DIP switches por defecto", "DIP switches padrão", "DIP switches par défaut",
		"Standard-DIP-Schalter"},
	{"Service button", "Botón de servicio", "Botão de serviço", "Bouton Service", "Service-Taste"},
	{"Test switch (the machine's own menu)", "Interruptor de test (menú de la máquina)", "Chave de teste (menu da máquina)",
		"Interrupteur Test (menu de la borne)", "Testschalter (Menü des Automaten)"},
	{"Reset", "Reiniciar", "Reiniciar", "Réinitialiser", "Reset"},
	{"Power cycle", "Apagar y encender", "Desligar e ligar", "Éteindre et rallumer", "Aus- und einschalten"},
	{"Back to the game list", "Volver a la lista de juegos", "Voltar à lista de jogos", "Retour à la liste des jeux",
		"Zurück zur Spieleliste"},
	{"Quit FBNeo PS5", "Salir de FBNeo PS5", "Sair do FBNeo PS5", "Quitter FBNeo PS5", "FBNeo PS5 beenden"},
	{"State saved.", "Estado guardado.", "Estado salvo.", "État sauvegardé.", "Spielstand gespeichert."},
	{"Could not save the state.", "No se pudo guardar el estado.", "Não foi possível salvar o estado.",
		"Impossible de sauvegarder l'état.", "Spielstand konnte nicht gespeichert werden."},
	{"This slot is empty.", "Esta ranura está vacía.", "Este slot está vazio.", "Cet emplacement est vide.",
		"Dieser Platz ist leer."},
	{"OK", "Aceptar", "OK", "OK", "OK"},
	{"Yes", "Sí", "Sim", "Oui", "Ja"},
	{"No", "No", "Não", "Non", "Nein"},
	// ---- the shelf
	{"Looking for games...", "Buscando juegos...", "Procurando jogos...", "Recherche des jeux...", "Suche Spiele..."},
	{"Looking for games... %zu found", "Buscando juegos... %zu encontrados", "Procurando jogos... %zu encontrados",
		"Recherche des jeux... %zu trouvés", "Suche Spiele... %zu gefunden"},
	{"Checking games... %d%% (%zu of %zu)", "Comprobando juegos... %d%% (%zu de %zu)",
		"Verificando jogos... %d%% (%zu de %zu)", "Vérification des jeux... %d%% (%zu sur %zu)",
		"Prüfe Spiele... %d%% (%zu von %zu)"},
	{"Downloading covers...", "Descargando portadas...", "Baixando capas...", "Téléchargement des jaquettes...",
		"Lade Cover..."},
	{"Quit FBNeo PS5?", "¿Salir de FBNeo PS5?", "Sair do FBNeo PS5?", "Quitter FBNeo PS5 ?", "FBNeo PS5 beenden?"},
	{"All games", "Todos los juegos", "Todos os jogos", "Tous les jeux", "Alle Spiele"},
	{"Classics", "Clásicos", "Clássicos", "Classiques", "Klassiker"},
	{"Other", "Otros", "Outros", "Autres", "Andere"},
	{"%d ROM(s) missing", "Faltan %d ROM(s)", "Faltam %d ROM(s)", "%d ROM(s) manquante(s)", "%d ROM(s) fehlen"},
	{"not working yet in FBNeo", "aún no funciona en FBNeo", "ainda não funciona no FBNeo",
		"ne fonctionne pas encore dans FBNeo", "läuft noch nicht in FBNeo"},
	{"no cover", "sin portada", "sem capa", "sans jaquette", "kein Cover"},
	{"No games found.", "No se encontraron juegos.", "Nenhum jogo encontrado.", "Aucun jeu trouvé.",
		"Keine Spiele gefunden."},
	{"Copy your arcade ROM sets (.zip, as named in FBNeo) to", "Copia tus sets de ROMs arcade (.zip, con el nombre de FBNeo) a",
		"Copie seus sets de ROMs de arcade (.zip, com o nome do FBNeo) para",
		"Copiez vos sets de ROM arcade (.zip, nommés comme dans FBNeo) dans",
		"Kopiere deine Arcade-ROM-Sets (.zip, benannt wie in FBNeo) nach"},
	{"/data/fbneo/roms   or   fbneo/roms  on a USB drive", "/data/fbneo/roms   o   fbneo/roms  en un USB",
		"/data/fbneo/roms   ou   fbneo/roms  num pendrive", "/data/fbneo/roms   ou   fbneo/roms  sur une clé USB",
		"/data/fbneo/roms   oder   fbneo/roms  auf einem USB-Stick"},
	{"with the BIOS sets they need beside them (neogeo.zip, pgm.zip...),",
		"con los sets de BIOS que necesiten al lado (neogeo.zip, pgm.zip...),",
		"com os sets de BIOS de que precisam ao lado (neogeo.zip, pgm.zip...),",
		"avec à côté les sets de BIOS dont ils ont besoin (neogeo.zip, pgm.zip...),",
		"mit den nötigen BIOS-Sets daneben (neogeo.zip, pgm.zip...),"},
	{"from a romset that matches FBNeo %s.  Then open FBNeo PS5 again.",
		"de un romset que coincida con FBNeo %s.  Luego vuelve a abrir FBNeo PS5.",
		"de um romset compatível com o FBNeo %s.  Depois abra o FBNeo PS5 de novo.",
		"d'un romset correspondant à FBNeo %s.  Puis rouvrez FBNeo PS5.",
		"aus einem Romset passend zu FBNeo %s.  Dann FBNeo PS5 neu öffnen."},
	{"(Some sets were found but are hidden: see Show incomplete sets in Settings.)",
		"(Se encontraron sets pero están ocultos: mira Mostrar sets incompletos en Ajustes.)",
		"(Foram encontrados sets, mas estão ocultos: veja Mostrar sets incompletos nas Configurações.)",
		"(Des sets ont été trouvés mais sont masqués : voir Afficher les sets incomplets dans les Paramètres.)",
		"(Einige Sets wurden gefunden, sind aber ausgeblendet: siehe Unvollständige Sets zeigen in den Einstellungen.)"},
	{"Downloading covers... %d left", "Descargando portadas... faltan %d", "Baixando capas... faltam %d",
		"Téléchargement des jaquettes... %d restantes", "Lade Cover... noch %d"},
	{"Downloading covers in the background... %d left", "Descargando portadas en segundo plano... faltan %d",
		"Baixando capas em segundo plano... faltam %d", "Téléchargement des jaquettes en arrière-plan... %d restantes",
		"Lade Cover im Hintergrund... noch %d"},
	{"Offline: covers next time", "Sin conexión: portadas la próxima vez", "Sem conexão: capas na próxima vez",
		"Hors ligne : jaquettes la prochaine fois", "Offline: Cover beim nächsten Mal"},
	{"Play", "Jugar", "Jogar", "Jouer", "Spielen"},
	{"Browse", "Explorar", "Navegar", "Parcourir", "Blättern"},
	{"Letter", "Letra", "Letra", "Lettre", "Buchstabe"},
	{"Maker", "Fabricante", "Fabricante", "Fabricant", "Hersteller"},
	{"Skip 10", "Saltar 10", "Pular 10", "Sauter 10", "10 weiter"},
	{"Get cover", "Obtener portada", "Baixar capa", "Récupérer la jaquette", "Cover holen"},
	{"Quit", "Salir", "Sair", "Quitter", "Beenden"},
	// ---- in a game
	{"State saved to slot %d", "Estado guardado en la ranura %d", "Estado salvo no slot %d",
		"État sauvegardé dans l'emplacement %d", "Spielstand auf Platz %d gespeichert"},
	{"Could not save the state", "No se pudo guardar el estado", "Não foi possível salvar o estado",
		"Impossible de sauvegarder l'état", "Spielstand konnte nicht gespeichert werden"},
	{"Slot %d is empty", "La ranura %d está vacía", "O slot %d está vazio", "L'emplacement %d est vide",
		"Platz %d ist leer"},
	{"State loaded from slot %d", "Estado cargado de la ranura %d", "Estado carregado do slot %d",
		"État chargé depuis l'emplacement %d", "Spielstand von Platz %d geladen"},
	{"Could not load the state: %s", "No se pudo cargar el estado: %s", "Não foi possível carregar o estado: %s",
		"Impossible de charger l'état : %s", "Spielstand konnte nicht geladen werden: %s"},
	{"not a FBNeo PS5 save of this kind", "no es un guardado de FBNeo PS5 de este tipo",
		"não é um save do FBNeo PS5 deste tipo", "pas une sauvegarde FBNeo PS5 de ce type",
		"kein FBNeo-PS5-Spielstand dieser Art"},
	{"a damaged save", "un guardado dañado", "um save danificado", "une sauvegarde endommagée",
		"ein beschädigter Spielstand"},
	{"a save of another game (%s)", "un guardado de otro juego (%s)", "um save de outro jogo (%s)",
		"une sauvegarde d'un autre jeu (%s)", "ein Spielstand eines anderen Spiels (%s)"},
	{"made by another version of the game's driver", "hecho con otra versión del driver del juego",
		"feito por outra versão do driver do jogo", "créée par une autre version du driver du jeu",
		"von einer anderen Version des Spieltreibers"},
	{"no game running", "ningún juego en marcha", "nenhum jogo rodando", "aucun jeu en cours", "kein Spiel läuft"},
	{"can't read it", "no se puede leer", "não dá para ler", "illisible", "nicht lesbar"},
	{"Could not start the game", "No se pudo iniciar el juego", "Não foi possível iniciar o jogo",
		"Impossible de lancer le jeu", "Das Spiel konnte nicht gestartet werden"},
	{"%s is not a ROM set FBNeo knows (the zip must keep its set name, as in FBNeo's lists).",
		"%s no es un set de ROMs que FBNeo conozca (el zip debe conservar el nombre del set, como en las listas de FBNeo).",
		"%s não é um set de ROMs que o FBNeo conheça (o zip deve manter o nome do set, como nas listas do FBNeo).",
		"%s n'est pas un set de ROM connu de FBNeo (le zip doit garder le nom du set, comme dans les listes de FBNeo).",
		"%s ist kein ROM-Set, das FBNeo kennt (die Zip muss ihren Set-Namen behalten, wie in FBNeos Listen)."},
	{"Missing ROMs for %s: %s. FBNeo PS5 looked in %s - the sets must match FBNeo's (%s).",
		"Faltan ROMs de %s: %s. FBNeo PS5 buscó en %s - los sets deben coincidir con los de FBNeo (%s).",
		"Faltam ROMs de %s: %s. O FBNeo PS5 procurou em %s - os sets devem corresponder aos do FBNeo (%s).",
		"ROM manquantes pour %s : %s. FBNeo PS5 a cherché dans %s - les sets doivent correspondre à ceux de FBNeo (%s).",
		"Fehlende ROMs für %s: %s. FBNeo PS5 hat in %s gesucht - die Sets müssen zu FBNeo passen (%s)."},
	{"FBNeo could not start %s (the ROMs may be from another version of the set).",
		"FBNeo no pudo iniciar %s (puede que las ROMs sean de otra versión del set).",
		"O FBNeo não conseguiu iniciar %s (as ROMs podem ser de outra versão do set).",
		"FBNeo n'a pas pu lancer %s (les ROM viennent peut-être d'une autre version du set).",
		"FBNeo konnte %s nicht starten (die ROMs stammen evtl. von einer anderen Version des Sets)."},
	{"FBNeo PS5 has no access to /data", "FBNeo PS5 no tiene acceso a /data", "O FBNeo PS5 não tem acesso a /data",
		"FBNeo PS5 n'a pas accès à /data", "FBNeo PS5 hat keinen Zugriff auf /data"},
	{"Send FBNeoPS5-v%s.elf with the Payload Manager (it unlocks the app), then open FBNeo PS5 again.",
		"Envía FBNeoPS5-v%s.elf con el Payload Manager (desbloquea la app) y vuelve a abrir FBNeo PS5.",
		"Envie o FBNeoPS5-v%s.elf com o Payload Manager (ele libera o app) e abra o FBNeo PS5 de novo.",
		"Envoyez FBNeoPS5-v%s.elf avec le Payload Manager (il débloque l'app), puis rouvrez FBNeo PS5.",
		"FBNeoPS5-v%s.elf mit dem Payload Manager senden (er entsperrt die App), dann FBNeo PS5 neu öffnen."},
	{"FBNeo PS5: could not start %s", "FBNeo PS5: no se pudo iniciar %s", "FBNeo PS5: não foi possível iniciar %s",
		"FBNeo PS5 : impossible de lancer %s", "FBNeo PS5: %s konnte nicht gestartet werden"},
	{"FBNeo PS5: the game could not be started again. %s", "FBNeo PS5: el juego no se pudo volver a iniciar. %s",
		"FBNeo PS5: não foi possível reiniciar o jogo. %s", "FBNeo PS5 : le jeu n'a pas pu être relancé. %s",
		"FBNeo PS5: das Spiel konnte nicht neu gestartet werden. %s"},
	{"FBNeo PS5: video failed to start (see %s/logs/boot.log)", "FBNeo PS5: no se pudo iniciar el vídeo (mira %s/logs/boot.log)",
		"FBNeo PS5: o vídeo não iniciou (veja %s/logs/boot.log)", "FBNeo PS5 : la vidéo n'a pas démarré (voir %s/logs/boot.log)",
		"FBNeo PS5: Video konnte nicht starten (siehe %s/logs/boot.log)"},
	{"FBNeo PS5: the emulator failed to start", "FBNeo PS5: no se pudo iniciar el emulador", "FBNeo PS5: o emulador não iniciou",
		"FBNeo PS5 : l'émulateur n'a pas démarré", "FBNeo PS5: der Emulator konnte nicht starten"},
};

const char* const kLanguageNames[] = {"English", "Español", "Português (Brasil)", "Français", "Deutsch"};

const std::unordered_map<std::string, const Entry*>& Index()
{
	static const std::unordered_map<std::string, const Entry*> index = [] {
		std::unordered_map<std::string, const Entry*> m;
		for (const Entry& e : kTable)
			m.emplace(e.en, &e);
		return m;
	}();
	return index;
}
} // namespace

const char* LanguageName(int lang)
{
	return lang >= 0 && lang < int(Language::Count) ? kLanguageNames[lang] : kLanguageNames[0];
}

const char* T(const char* english)
{
	const int lang = Config().language;
	if (lang <= 0 || lang >= int(Language::Count) || !english)
		return english;
	const auto& index = Index();
	const auto it = index.find(english);
	if (it == index.end())
	{
#ifndef __PROSPERO__
		// the host tests: every text shown without a translation, once (FBNEO_HOST_I18N_MISSING=<file>)
		if (const char* path = getenv("FBNEO_HOST_I18N_MISSING"))
		{
			static std::mutex m;
			static std::set<std::string> seen;
			std::lock_guard<std::mutex> lock(m);
			if (seen.insert(english).second)
				if (FILE* f = fopen(path, "a"))
				{
					fprintf(f, "%s\n", english);
					fclose(f);
				}
		}
#endif
		return english;
	}
	const Entry& e = *it->second;
	const char* t = lang == 1 ? e.es : lang == 2 ? e.pt : lang == 3 ? e.fr : e.de;
	return t && *t ? t : english;
}
} // namespace fe
