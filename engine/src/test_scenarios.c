#include "test_scenarios.h"
#include <string.h>

// Bytes of the save area, from $7E:F000.
#define BOW(v)          { 0x340, v }  // 2 bow and arrows, 4 silver arrows
#define BOOMERANG(v)    { 0x341, v }  // 1 blue, 2 red
#define HOOKSHOT        { 0x342, 1 }
#define BOMBS(v)        { 0x343, v }
#define POWDER          { 0x344, 2 }
#define FIRE_ROD        { 0x345, 1 }
#define ICE_ROD         { 0x346, 1 }
#define MEDALLIONS      { 0x347, 1 }, { 0x348, 1 }, { 0x349, 1 }
#define LAMP            { 0x34a, 1 }
#define HAMMER          { 0x34b, 1 }
#define FLUTE           { 0x34c, 3 }  // the bird answers it
#define NET             { 0x34d, 1 }
#define BOOK            { 0x34e, 1 }
#define SOMARIA         { 0x350, 1 }
#define BYRNA           { 0x351, 1 }
#define CAPE            { 0x352, 1 }
#define MIRROR          { 0x353, 2 }
#define GLOVES(v)       { 0x354, v }  // 1 power glove, 2 titan's mitt
#define BOOTS           { 0x355, 1 }
#define FLIPPERS        { 0x356, 1 }
#define PEARL(v)        { 0x357, v }
#define SWORD(v)        { 0x359, v }  // 1 fighter's .. 4 golden
#define SHIELD(v)       { 0x35a, v }  // 0 none .. 3 mirror shield
#define ARMOR(v)        { 0x35b, v }  // 0 green, 1 blue, 2 red
// Contents: 2 empty, 3 red potion, 4 green, 5 blue, 6 fairy, 7 bee.
#define BOTTLES(a, b, c, d) { 0x34f, 1 }, { 0x35c, a }, { 0x35d, b }, { 0x35e, c }, { 0x35f, d }
#define RUPEES(v)       { 0x360, (v) & 0xff }, { 0x361, (v) >> 8 }, \
                        { 0x362, (v) & 0xff }, { 0x363, (v) >> 8 }
// Hearts: capacity in hearts, life in eighths of a heart.
#define HEARTS(cap, life) { 0x36c, (cap) * 8 }, { 0x36d, life }
#define MAGIC(v)        { 0x36e, v }  // 0x80 full
#define MAGIC_USE(v)    { 0x37b, v }  // 0 normal, 1 half, 2 quarter
#define ARROWS(v)       { 0x377, v }

#define FULL_KIT \
  BOW(4), BOOMERANG(2), HOOKSHOT, BOMBS(30), POWDER, FIRE_ROD, ICE_ROD, \
  MEDALLIONS, LAMP, HAMMER, FLUTE, NET, BOOK, SOMARIA, BYRNA, CAPE, MIRROR, \
  GLOVES(2), BOOTS, FLIPPERS, PEARL(1), SWORD(4), SHIELD(3), ARMOR(2), \
  BOTTLES(6, 3, 5, 7), RUPEES(999), HEARTS(20, 160), MAGIC(0x80), ARROWS(50), \
  { 0x370, 7 }, { 0x371, 7 }

#define E kTestScenario_Entrance
#define X kTestScenario_Exit

const TestScenario kTestScenarios[] = {
  { "Casa de Link", "Tras la fuga del castillo. Equipo inicial.",
    0, E, 1, false, { { 0 } } },
  { "Santuario", "Zelda a salvo. Primeras horas de la aventura.",
    0, E, 2, false, { { 0 } } },
  { "Castillo de Hyrule", "Planta principal del castillo.",
    0, E, 4, false, { { 0 } } },
  { "Palacio del Este", "Primera mazmorra con el equipo inicial.",
    0, E, 8, false, { { 0 } } },
  { "Este al límite", "Medio corazón y sin magia.",
    0, E, 8, false, { HEARTS(3, 4), MAGIC(0) } },
  { "Este: exterior", "Arco y flechas en campo abierto.",
    1, X, 0xc9, false, { { 0 } } },
  { "Palacio Desierto", "Libro de Mudora. Botas de Pegaso.",
    1, E, 9, false, { BOOK, BOOTS } },
  { "Desierto: exterior", "Exterior del desierto con guantes.",
    2, X, 0x84, false, { { 0 } } },
  { "Torre de Hera", "Montaña de la Muerte con espejo mágico.",
    2, E, 51, false, { MIRROR } },
  { "Cima de la Montaña", "Perla de Luna recien conseguida.",
    3, X, 0x77, false, { { 0 } } },
  { "Bosque Perdido", "Camino a la Espada Maestra.",
    3, X, 0xe1, false, { { 0 } } },
  { "Torre de Agahnim", "Espada Maestra y capa mágica.",
    3, E, 36, false, { SWORD(2), CAPE, MAGIC(0x80) } },
  { "Pirámide del Poder", "Llegada al Mundo Oscuro.",
    4, X, 0x20, true, { { 0 } } },
  { "Palacio Oscuridad", "Primera mazmorra del Mundo Oscuro.",
    4, E, 38, true, { { 0 } } },
  { "Oscuridad al límite", "Medio corazón y sin magia.",
    4, E, 38, true, { HEARTS(6, 4), MAGIC(0) } },
  { "Link conejo", "Mundo Oscuro sin la Perla de Luna.",
    4, X, 0x4a, true, { PEARL(0) } },
  { "Palacio del Pantano", "Aletas y martillo.",
    5, E, 37, true, { { 0 } } },
  { "Pantano: exterior", "Pantano con el espejo para volver.",
    5, X, 0x28, true, { { 0 } } },
  { "Bosque Calavera", "Gancho y varias entradas.",
    6, E, 42, true, { { 0 } } },
  { "Calavera: exterior", "Bosque oscuro con la vara de fuego.",
    6, X, 0x58, true, { { 0 } } },
  { "Pueblo Ladrones", "Mazmorra de los guantes de titán.",
    7, E, 52, true, { { 0 } } },
  { "Pueblo Proscritos", "Exterior con tiendas y casas.",
    7, X, 0xdb, true, { { 0 } } },
  { "Palacio de Hielo", "Vara de fuego y guantes de titán.",
    8, E, 45, true, { { 0 } } },
  { "Lago de Hielo", "Exterior helado con aletas.",
    8, X, 0x0e, true, { { 0 } } },
  { "Palacio Miseria", "Mazmorra del bastón de Somaria.",
    9, E, 39, true, { { 0 } } },
  { "Ciénaga Miseria", "Exterior bajo la lluvia.",
    9, X, 0x98, true, { { 0 } } },
  { "Roca Tortuga", "Bastones de Somaria y de Byrna.",
    10, E, 53, true, { BYRNA } },
  { "Montaña Oscura", "Exterior de la Roca Tortuga.",
    10, X, 0xd6, true, { { 0 } } },
  { "Torre de Ganon", "Siete cristales, última mazmorra.",
    11, E, 55, true, { { 0 } } },
  { "Cumbre oscura", "Exterior de la Torre de Ganon.",
    11, X, 0x0c, true, { { 0 } } },
  { "Batalla con Ganon", "Flechas de plata y espada templada.",
    11, E, 123, true, { BOW(4), ARROWS(50), SWORD(3) } },
  { "Ganon, equipo débil", "Espada Maestra, 3 corazones, túnica verde.",
    11, E, 123, true, { BOW(4), ARROWS(30), SWORD(2), SHIELD(1), ARMOR(0),
                        HEARTS(3, 24) } },
  { "Equipo: Castillo", "Todo el inventario y 20 corazones.",
    11, X, 0x61, false, { FULL_KIT } },
  { "Equipo: Kakariko", "Todo el inventario en la aldea.",
    11, X, 0x2f, false, { FULL_KIT } },
  { "Equipo: Desierto", "Todo el inventario en el desierto.",
    11, X, 0x84, false, { FULL_KIT } },
  { "Equipo: Pirámide", "Pirámide con todo el inventario.",
    11, X, 0x20, true, { FULL_KIT } },
  { "Sin magia", "Todo el inventario con la barra vacía.",
    11, E, 38, true, { FULL_KIT, MAGIC(0) } },
  { "Magia a un cuarto", "Consumo de magia mínimo y botellas llenas.",
    11, E, 53, true, { FULL_KIT, MAGIC_USE(2) } },
  { "1 corazón, 4 hadas", "Prueba de muerte y hadas en botella.",
    11, E, 55, true, { FULL_KIT, HEARTS(20, 8), BOTTLES(6, 6, 6, 6) } },
  { "Cueva del Viejo", "Montaña de noche con la lámpara.",
    1, E, 6, false, { LAMP } },
};

const int kTestScenarioCount = sizeof(kTestScenarios) / sizeof(kTestScenarios[0]);

// Authored post-escape progression, independent of reference saves.
void TestScenario_InitSram(int index, uint8 *sram) {
  memset(sram, 0, 0x500);
  const TestScenario *s = &kTestScenarios[index];
  int chapter = s->chapter;
  sram[0x3e5] = 0xaa; sram[0x3e6] = 0x55;
  sram[0x20d] = sram[0x20f] = 0xf0;
  sram[0x403] = sram[0x404] = 0xff;
  sram[0x359] = chapter >= 4 ? 2 : 1;
  sram[0x35a] = 1; sram[0x34a] = 1;
  sram[0x36c] = sram[0x36d] = (3 + chapter) * 8;
  sram[0x36e] = 0x80; sram[0x379] = 0xf8;
  sram[0x3c5] = chapter >= 4 ? 3 : 2;
  sram[0x3c6] = 0x14; sram[0x3c8] = 1;
  sram[0x3ca] = s->dark_world ? 0x40 : 0;
  sram[0x341] = 1; sram[0x343] = 10;
  if (chapter >= 1) { sram[0x340] = 2; sram[0x377] = 30; sram[0x355] = 1; sram[0x34e] = 1; }
  if (chapter >= 2) sram[0x354] = 1;
  if (chapter >= 3) { sram[0x353] = 2; sram[0x357] = 1; }
  sram[0x374] = chapter >= 3 ? 7 : chapter >= 2 ? 6 : chapter ? 4 : 0;
  if (chapter >= 5) { sram[0x34b] = 1; sram[0x356] = 1; }
  if (chapter >= 6) sram[0x342] = 1;
  if (chapter >= 7) sram[0x345] = 1;
  if (chapter >= 8) sram[0x354] = 2;
  if (chapter >= 9) { sram[0x34c] = 3; sram[0x348] = 1; }
  if (chapter >= 10) { sram[0x350] = 1; sram[0x346] = 1; sram[0x349] = 1; }
  static const uint8 crystals[] = {0,0,0,0,0,2,0x12,0x52,0x56,0x57,0x77,0x7f};
  sram[0x37a] = crystals[chapter];
  for (const TestScenarioSet *set = s->sets; set->addr; set++) sram[set->addr] = set->value;
  for (int i=0;i<6;i++) { sram[0x3d9+i*2]=i<4 ? (uint8[]){0x23,4,0x22,0x23}[i] : 0xa9; }
  uint16 checksum=0x5a5a;
  for (int i=0;i<0x4fe;i+=2) checksum -= sram[i] | sram[i+1]<<8;
  sram[0x4fe]=checksum; sram[0x4ff]=checksum>>8;
}

static const char *const kScenarioNames[7][40] = {
  {"Link's house", "Sanctuary", "Hyrule Castle", "Eastern Palace", "Eastern: low health", "Eastern: outside", "Desert Palace", "Desert: outside", "Tower of Hera", "Mountain summit", "Lost Woods", "Agahnim's Tower", "Pyramid", "Palace of Darkness", "Darkness: low health", "Bunny Link", "Swamp Palace", "Swamp: outside", "Skull Woods", "Skull: outside", "Thieves' Town", "Village of Outcasts", "Ice Palace", "Frozen lake", "Misery Mire", "Mire: outside", "Turtle Rock", "Dark Mountain", "Ganon's Tower", "Dark summit", "Ganon", "Ganon: weak gear", "Gear: Castle", "Gear: Kakariko", "Gear: Desert", "Gear: Pyramid", "No magic", "Quarter magic", "1 heart, 4 fairies", "Old man's cave"},
  {"Maison de Link", "Sanctuaire", "Château d'Hyrule", "Palais de l'Est", "Est : vie faible", "Est : extérieur", "Palais du Désert", "Désert : extérieur", "Tour d'Héra", "Sommet", "Bois Perdus", "Tour d'Agahnim", "Pyramide", "Palais des Ténèbres", "Ténèbres : vie faible", "Link lapin", "Palais des Marais", "Marais : extérieur", "Bois des Squelettes", "Squelettes : dehors", "Village des Voleurs", "Village des Bannis", "Palais de Glace", "Lac gelé", "Bourbier Souffrance", "Bourbier : dehors", "Rocher Tortue", "Montagne sombre", "Tour de Ganon", "Sommet sombre", "Ganon", "Ganon : équipement faible", "Équipement : Château", "Équipement : Cocorico", "Équipement : Désert", "Équipement : Pyramide", "Sans magie", "Quart de magie", "1 coeur, 4 fées", "Grotte du vieillard"},
  {"Casa de Link", "Santuario", "Castillo de Hyrule", "Palacio del Este", "Este al límite", "Este: exterior", "Palacio Desierto", "Desierto: exterior", "Torre de Hera", "Cima de la Montaña", "Bosque Perdido", "Torre de Agahnim", "Pirámide del Poder", "Palacio Oscuridad", "Oscuridad al límite", "Link conejo", "Palacio del Pantano", "Pantano: exterior", "Bosque Calavera", "Calavera: exterior", "Pueblo Ladrones", "Pueblo Proscritos", "Palacio de Hielo", "Lago de Hielo", "Palacio Miseria", "Ciénaga Miseria", "Roca Tortuga", "Montaña Oscura", "Torre de Ganon", "Cumbre oscura", "Batalla con Ganon", "Ganon: equipo débil", "Equipo: Castillo", "Equipo: Kakariko", "Equipo: Desierto", "Equipo: Pirámide", "Sin magia", "Magia a un cuarto", "1 corazón, 4 hadas", "Cueva del Viejo"},
  {"Casa de Link", "Santuário", "Castelo de Hyrule", "Palácio do Leste", "Leste: pouca vida", "Leste: exterior", "Palácio do Deserto", "Deserto: exterior", "Torre de Hera", "Topo da Montanha", "Bosque Perdido", "Torre de Agahnim", "Pirâmide", "Palácio das Trevas", "Trevas: pouca vida", "Link coelho", "Palácio do Pântano", "Pântano: exterior", "Bosque Caveira", "Caveira: exterior", "Vila dos Ladrões", "Vila dos Exilados", "Palácio de Gelo", "Lago congelado", "Pântano da Miséria", "Miséria: exterior", "Rocha Tartaruga", "Montanha Sombria", "Torre de Ganon", "Cume sombrio", "Ganon", "Ganon: pouco equipamento", "Equipamento: Castelo", "Equipamento: Kakariko", "Equipamento: Deserto", "Equipamento: Pirâmide", "Sem magia", "Um quarto de magia", "1 coração, 4 fadas", "Caverna do ancião"},
  {"Casa di Link", "Santuario", "Castello di Hyrule", "Palazzo dell'Est", "Est: poca vita", "Est: esterno", "Palazzo del Deserto", "Deserto: esterno", "Torre di Hera", "Cima del Monte", "Bosco Perduto", "Torre di Agahnim", "Piramide", "Palazzo Oscuro", "Oscurità: poca vita", "Link coniglio", "Palazzo della Palude", "Palude: esterno", "Bosco degli Scheletri", "Scheletri: esterno", "Villaggio dei Ladri", "Villaggio degli Esuli", "Palazzo di Ghiaccio", "Lago ghiacciato", "Palude della Miseria", "Miseria: esterno", "Roccia Tartaruga", "Monte Oscuro", "Torre di Ganon", "Cima oscura", "Ganon", "Ganon: equip. debole", "Equipaggiamento: Castello", "Equipaggiamento: Kakariko", "Equipaggiamento: Deserto", "Equipaggiamento: Piramide", "Senza magia", "Un quarto di magia", "1 cuore, 4 fate", "Grotta dell'anziano"},
  {"Links Haus", "Kathedrale", "Schloss Hyrule", "Ostpalast", "Osten: wenig Leben", "Osten: Aussenbereich", "Wüstenpalast", "Wüste: Aussenbereich", "Heras Turm", "Berggipfel", "Verlorene Wälder", "Agahnims Turm", "Pyramide", "Schattenpalast", "Schatten: wenig Leben", "Hasen-Link", "Sumpfpalast", "Sumpf: Aussenbereich", "Schädelwald", "Schädel: Aussenbereich", "Diebesversteck", "Dorf der Verbannten", "Eispalast", "Eissee", "Elendssumpf", "Elend: Aussenbereich", "Schildkrötenfelsen", "Schattenberg", "Ganons Turm", "Dunkler Gipfel", "Ganon", "Ganon: schwache Ausrüstung", "Ausrüstung: Schloss", "Ausrüstung: Kakariko", "Ausrüstung: Wüste", "Ausrüstung: Pyramide", "Ohne Magie", "Viertelmagie", "1 Herz, 4 Feen", "Höhle des Alten"},
  {"Дом Линка", "Святилище", "Замок Хайрул", "Восточный дворец", "Восток: мало здоровья", "Восток: снаружи", "Дворец пустыни", "Пустыня: снаружи", "Башня Геры", "Вершина горы", "Затерянный лес", "Башня Аганима", "Пирамида", "Дворец тьмы", "Тьма: мало здоровья", "Линк-кролик", "Болотный дворец", "Болото: снаружи", "Лес черепов", "Черепа: снаружи", "Город воров", "Деревня изгнанников", "Ледяной дворец", "Ледяное озеро", "Трясина страданий", "Трясина: снаружи", "Скала черепахи", "Тёмная гора", "Башня Ганона", "Тёмная вершина", "Ганон", "Ганон: слабое снаряжение", "Снаряжение: замок", "Снаряжение: Какарико", "Снаряжение: пустыня", "Снаряжение: пирамида", "Без магии", "Четверть магии", "1 сердце, 4 феи", "Пещера старика"},
};
static const char *const kScenarioDetails[][7] = {
  {"POST-ESCAPE EQUIPMENT", "ÉQUIPEMENT APRÈS LA FUITE", "EQUIPO TRAS LA FUGA", "EQUIPAMENTO APÓS A FUGA", "EQUIPAGGIAMENTO DOPO LA FUGA", "AUSRÜSTUNG NACH DER FLUCHT", "СНАРЯЖЕНИЕ ПОСЛЕ ПОБЕГА"},
  {"DUNGEON AND CHAPTER EQUIPMENT", "DONJON ET ÉQUIPEMENT DU CHAPITRE", "MAZMORRA Y EQUIPO DEL CAPÍTULO", "MASMORRA E EQUIPAMENTO DO CAPÍTULO", "DUNGEON E OGGETTI DEL CAPITOLO", "VERLIES MIT KAPITELAUSRÜSTUNG", "ПОДЗЕМЕЛЬЕ И СНАРЯЖЕНИЕ ГЛАВЫ"},
  {"OUTSIDE WITH CHAPTER EQUIPMENT", "EXTÉRIEUR AVEC ÉQUIPEMENT DU CHAPITRE", "EXTERIOR CON EQUIPO DEL CAPÍTULO", "EXTERIOR COM EQUIPAMENTO DO CAPÍTULO", "ESTERNO CON OGGETTI DEL CAPITOLO", "AUSSEN MIT KAPITELAUSRÜSTUNG", "СНАРУЖИ СО СНАРЯЖЕНИЕМ ГЛАВЫ"},
  {"HALF A HEART, NO MAGIC", "UN DEMI-COEUR, SANS MAGIE", "MEDIO CORAZÓN, SIN MAGIA", "MEIO CORAÇÃO, SEM MAGIA", "MEZZO CUORE, SENZA MAGIA", "HALBES HERZ, OHNE MAGIE", "ПОЛСЕРДЦА, БЕЗ МАГИИ"},
  {"DARK WORLD WITHOUT MOON PEARL", "MONDE SOMBRE SANS PERLE DE LUNE", "MUNDO OSCURO SIN PERLA DE LUNA", "MUNDO SOMBRIO SEM PÉROLA DA LUA", "MONDO OSCURO SENZA PERLA LUNARE", "SCHATTENWELT OHNE MONDPERLE", "ТЁМНЫЙ МИР БЕЗ ЛУННОЙ ЖЕМЧУЖИНЫ"},
  {"FULL INVENTORY, 20 HEARTS", "INVENTAIRE COMPLET, 20 COEURS", "INVENTARIO COMPLETO, 20 CORAZONES", "INVENTÁRIO COMPLETO, 20 CORAÇÕES", "INVENTARIO COMPLETO, 20 CUORI", "VOLLES INVENTAR, 20 HERZEN", "ВСЕ ПРЕДМЕТЫ, 20 СЕРДЕЦ"},
  {"FULL INVENTORY, EMPTY MAGIC", "INVENTAIRE COMPLET, MAGIE VIDE", "INVENTARIO COMPLETO, MAGIA VACÍA", "INVENTÁRIO COMPLETO, SEM MAGIA", "INVENTARIO COMPLETO, MAGIA VUOTA", "VOLLES INVENTAR, LEERE MAGIE", "ВСЕ ПРЕДМЕТЫ, МАГИЯ ПУСТА"},
  {"QUARTER MAGIC CONSUMPTION", "CONSOMMATION DE MAGIE RÉDUITE AU QUART", "CONSUMO DE MAGIA A UN CUARTO", "CONSUMO DE MAGIA A UM QUARTO", "CONSUMO DI MAGIA A UN QUARTO", "EIN VIERTEL MAGIEVERBRAUCH", "РАСХОД МАГИИ В ЧЕТВЕРТЬ"},
  {"ONE HEART AND FOUR BOTTLED FAIRIES", "UN COEUR ET QUATRE FÉES EN FLACON", "UN CORAZÓN Y CUATRO HADAS", "UM CORAÇÃO E QUATRO FADAS", "UN CUORE E QUATTRO FATE", "EIN HERZ UND VIER FEEN", "ОДНО СЕРДЦЕ И ЧЕТЫРЕ ФЕИ"},
  {"MASTER SWORD, THREE HEARTS", "ÉPÉE DE LÉGENDE, TROIS COEURS", "ESPADA MAESTRA, TRES CORAZONES", "ESPADA MESTRA, TRÊS CORAÇÕES", "SPADA SUPREMA, TRE CUORI", "MASTER-SCHWERT, DREI HERZEN", "МАСТЕР-МЕЧ, ТРИ СЕРДЦА"},
};
const char *TestScenario_Name(int index, int language) {
  if (index<0 || index>=kTestScenarioCount) return "";
  return kScenarioNames[language>=0 && language<7 ? language : 0][index];
}
const char *TestScenario_Detail(int index, int language) {
  if (index<0 || index>=kTestScenarioCount) return "";
  int kind = index<3 ? 0 : kTestScenarios[index].kind == kTestScenario_Exit ? 2 : 1;
  if (index==4 || index==14) kind=3;
  if (index==15) kind=4;
  if (index>=32 && index<=35) kind=5;
  if (index==36) kind=6;
  if (index==37) kind=7;
  if (index==38) kind=8;
  if (index==31) kind=9;
  return kScenarioDetails[kind][language>=0 && language<7 ? language : 0];
}
