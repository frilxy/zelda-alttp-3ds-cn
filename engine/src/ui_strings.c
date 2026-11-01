// Native interface strings. Keep short labels readable at 320x240.
#include <string.h>

#include "ui_strings.h"

static const char *const kUiStrings[kUiStringCount][kUiLanguageCount] = {
  [kUi_QolDescFastSwitch] = {"L AND R CYCLE THROUGH OWNED ITEMS.", "L ET R FONT DÉFILER LES OBJETS POSSÉDÉS.", "L Y R CAMBIAN ENTRE LOS OBJETOS QUE TIENES.", "L E R ALTERNAM ENTRE OS ITENS QUE VOCÊ TEM.", "L E R CAMBIANO GLI OGGETTI POSSEDUTI.", "L UND R WECHSELN ZWISCHEN DEINEN GEGENSTÄNDEN.", "L И R ПЕРЕКЛЮЧАЮТ ИМЕЮЩИЕСЯ ПРЕДМЕТЫ.", "L/R 切换已拥有的道具。" },
  [kUi_QolDescPegasus] = {"TURN WHILE DASHING AND KEEP RUNNING AFTER A TRANSITION.", "TOURNEZ EN COURANT ET GARDEZ VOTRE ÉLAN ENTRE LES ZONES.", "PERMITE GIRAR AL CORRER Y SEGUIR CORRIENDO AL CAMBIAR DE ZONA.", "PERMITE VIRAR NA CORRIDA E CONTINUAR AO MUDAR DE ÁREA.", "GIRA DURANTE LA CORSA E CONTINUA TRA LE AREE.", "BEIM RENNEN ABBIEGEN UND NACH EINEM GEBIETSWECHSEL WEITERRENNEN.", "ПОВОРОТЫ ВО ВРЕМЯ БЕГА И БЕГ ПОСЛЕ ПЕРЕХОДА МЕЖДУ ЗОНАМИ.", "冲刺时可转向，换区后继续奔跑。" },
  [kUi_QolDescKakariko] = {"OPENS A SHORTCUT NEAR KAKARIKO WITH THE REQUIRED GLOVE.", "OUVRE UN RACCOURCI PRÈS DE COCORICO AVEC LE GANT REQUIS.", "ABRE UN ATAJO JUNTO A KAKARIKO CON EL GUANTE NECESARIO.", "ABRE UM ATALHO PERTO DE KAKARIKO COM A LUVA NECESSÁRIA.", "APRE UNA SCORCIATOIA VICINO A KAKARIKO CON IL GUANTO RICHIESTO.", "ÖFFNET MIT DEM PASSENDEN HANDSCHUH EINE ABKÜRZUNG BEI KAKARIKO.", "ОТКРЫВАЕТ КОРОТКИЙ ПУТЬ У КАКАРИКО ПРИ НАЛИЧИИ НУЖНОЙ ПЕРЧАТКИ.", "持有手套时在卡卡利科旁开启捷径。" },
  [kUi_QolDescDimFlashes] = {"REDUCES BRIGHT FLASHES DURING EFFECTS AND TRANSITIONS.", "ATTÉNUE LES FLASHES DES EFFETS ET TRANSITIONS.", "REDUCE LOS DESTELLOS DE LOS EFECTOS Y LAS TRANSICIONES.", "REDUZ OS CLARÕES DOS EFEITOS E DAS TRANSIÇÕES.", "RIDUCE I LAMPEGGI DI EFFETTI E TRANSIZIONI.", "VERRINGERT HELLE BLITZE BEI EFFEKTEN UND ÜBERGÄNGEN.", "УМЕНЬШАЕТ ЯРКИЕ ВСПЫШКИ ЭФФЕКТОВ И ПЕРЕХОДОВ.", "减弱特效与切换时的强光闪烁。" },
  [kUi_QolDescFairyHearts] = {"FAIRIES RESTORE EIGHT HEARTS, INCLUDING WHEN REVIVING LINK.", "LES FÉES RENDENT HUIT COEURS, MÊME EN RESSUSCITANT LINK.", "LAS HADAS RECUPERAN OCHO CORAZONES, TAMBIÉN AL REVIVIR A LINK.", "AS FADAS RECUPERAM OITO CORAÇÕES, INCLUSIVE AO REVIVER LINK.", "LE FATE RIPRISTINANO OTTO CUORI ANCHE QUANDO RIANIMANO LINK.", "FEEN HEILEN ACHT HERZEN, AUCH BEIM WIEDERBELEBEN VON LINK.", "ФЕИ ВОССТАНАВЛИВАЮТ ВОСЕМЬ СЕРДЕЦ, В ТОМ ЧИСЛЕ ПРИ ВОЗРОЖДЕНИИ.", "妖精恢复八颗心，复活时同样有效。" },
  [kUi_QolDescSwordPickup] = {"COLLECT DROPPED ITEMS BY TOUCHING THEM WITH THE SWORD.", "RAMASSEZ LES OBJETS AU SOL AVEC VOTRE ÉPÉE.", "RECOGE OBJETOS CAÍDOS TOCÁNDOLOS CON LA ESPADA.", "RECOLHA ITENS CAÍDOS TOCANDO NELES COM A ESPADA.", "RACCOGLI GLI OGGETTI A TERRA CON LA SPADA.", "SAMMLE GEGENSTÄNDE AM BODEN MIT DEM SCHWERT EIN.", "СОБИРАЙТЕ ЛЕЖАЩИЕ ПРЕДМЕТЫ, КАСАЯСЬ ИХ МЕЧОМ.", "用剑触碰即可拾取掉落的道具。" },
  [kUi_QolDescBrightRooms] = {"SHOWS THE WHOLE DARK ROOM. DOES NOT CHANGE PUZZLES OR LAMP OWNERSHIP.", "ÉCLAIRE LA SALLE ENTIÈRE SANS MODIFIER LES ÉNIGMES NI DONNER LA LAMPE.", "MUESTRA TODA LA SALA OSCURA. NO CAMBIA LOS PUZLES NI TE DA LA LÁMPARA.", "MOSTRA TODA A SALA ESCURA SEM MUDAR ENIGMAS NEM DAR A LANTERNA.", "MOSTRA TUTTA LA STANZA BUIA SENZA CAMBIARE ENIGMI O DARE LA LANTERNA.", "ZEIGT DUNKLE RÄUME GANZ AN. RÄTSEL UND LAMPENBESITZ BLEIBEN GLEICH.", "ПОКАЗЫВАЕТ ТЁМНУЮ КОМНАТУ ЦЕЛИКОМ. НЕ МЕНЯЕТ ЗАГАДКИ И НЕ ДАЁТ ЛАМПУ.", "照亮整个黑暗房间，不改谜题与油灯。" },
  [kUi_QolDescMuteBeep] = {"DISABLES THE ANNOYING BEEP THAT SOUNDS WHEN HEALTH IS LOW.", "COUPE LE BIP GÊNANT QUI SONNE QUAND LA VIE EST FAIBLE.", "DESACTIVA EL PITIDO MOLESTO QUE SUENA CUANDO QUEDA POCA VIDA.", "DESATIVA O APITO INCÔMODO QUE SOA QUANDO RESTA POUCA VIDA.", "DISATTIVA IL FASTIDIOSO SEGNALE QUANDO RESTA POCA VITA.", "SCHALTET DEN STÖRENDEN WARNTON BEI WENIG LEBEN AUS.", "ОТКЛЮЧАЕТ НАЗОЙЛИВЫЙ СИГНАЛ, КОГДА ОСТАЁТСЯ МАЛО ЗДОРОВЬЯ.", "关闭低血量时恼人的提示音。" },
  [kUi_QolDescRepeatBlocks] = {"ALLOWS DUNGEON BLOCKS TO BE PUSHED MORE THAN ONCE.", "PERMET DE POUSSER PLUSIEURS FOIS LES BLOCS DES DONJONS.", "PERMITE EMPUJAR VARIAS VECES LOS BLOQUES DE LAS MAZMORRAS.", "PERMITE EMPURRAR MAIS DE UMA VEZ OS BLOCOS DOS CALABOUÇOS.", "PERMETTE DI SPOSTARE PIÙ VOLTE I BLOCCHI DEI DUNGEON.", "BLÖCKE IN VERLIESEN LASSEN SICH MEHRFACH SCHIEBEN.", "ПОЗВОЛЯЕТ НЕСКОЛЬКО РАЗ ДВИГАТЬ БЛОКИ В ПОДЗЕМЕЛЬЯХ.", "迷宫中的方块可以反复推动。" },
  [kUi_QolDescBombs99] = {"RAISES BOMB CAPACITY TO 99 WITHOUT GIVING EXTRA BOMBS.", "PORTE LA CAPACITÉ À 99 BOMBES SANS EN DONNER.", "AUMENTA LA CAPACIDAD A 99 BOMBAS. NO REGALA BOMBAS.", "AUMENTA A CAPACIDADE PARA 99 BOMBAS SEM DAR BOMBAS EXTRAS.", "AUMENTA LA CAPACITÀ A 99 BOMBE SENZA REGALARNE.", "ERHÖHT DIE KAPAZITÄT AUF 99 BOMBEN, OHNE BOMBEN ZU VERGEBEN.", "УВЕЛИЧИВАЕТ ВМЕСТИМОСТЬ ДО 99 БОМБ, НЕ ДОБАВЛЯЯ САМИ БОМБЫ.", "炸弹上限提至 99，不额外赠送炸弹。" },
  [kUi_QolDescArrows99] = {"RAISES ARROW CAPACITY TO 99 WITHOUT GIVING EXTRA ARROWS.", "PORTE LA CAPACITÉ À 99 FLÈCHES SANS EN DONNER.", "AUMENTA LA CAPACIDAD A 99 FLECHAS. NO REGALA FLECHAS.", "AUMENTA A CAPACIDADE PARA 99 FLECHAS SEM DAR FLECHAS EXTRAS.", "AUMENTA LA CAPACITÀ A 99 FRECCE SENZA REGALARNE.", "ERHÖHT DIE KAPAZITÄT AUF 99 PFEILE, OHNE PFEILE ZU VERGEBEN.", "УВЕЛИЧИВАЕТ ВМЕСТИМОСТЬ ДО 99 СТРЕЛ, НЕ ДОБАВЛЯЯ САМИ СТРЕЛЫ.", "箭上限提至 99，不额外赠送箭。" },
  [kUi_QolDescWallet9999] = {"RAISES WALLET CAPACITY TO 9999 RUPEES WITHOUT GIVING MONEY.", "PORTE LA BOURSE À 9999 RUBIS SANS DONNER D'ARGENT.", "AUMENTA LA CARTERA A 9999 RUPIAS. NO REGALA DINERO.", "AUMENTA A CARTEIRA PARA 9999 RÚPIAS SEM DAR DINHEIRO.", "AUMENTA LA BORSA A 9999 RUPIE SENZA REGALARE DENARO.", "ERHÖHT DEN GELDBEUTEL AUF 9999 RUBINE, OHNE GELD ZU VERGEBEN.", "УВЕЛИЧИВАЕТ КОШЕЛЁК ДО 9999 РУПИЙ, НЕ ДОБАВЛЯЯ ДЕНЬГИ.", "钱包上限提至 9999，不赠送卢比。" },
  [kUi_QolDescInventory] = {"USES 24 SLOTS, WITH FOUR SEPARATE BOTTLES AND A SHOVEL SLOT.", "24 CASES AVEC QUATRE FLACONS SÉPARÉS ET UNE CASE POUR LA PELLE.", "USA 24 CASILLAS, CON CUATRO BOTELLAS SEPARADAS Y UNA CASILLA PARA LA PALA.", "USA 24 ESPAÇOS, COM QUATRO GARRAFAS SEPARADAS E UM ESPAÇO PARA A PÁ.", "USA 24 CASELLE CON QUATTRO AMPOLLE SEPARATE E UNA CASELLA PER LA PALA.", "24 FELDER MIT VIER EINZELNEN FLASCHEN UND EINEM SCHAUFELFELD.", "24 ЯЧЕЙКИ, ЧЕТЫРЕ ОТДЕЛЬНЫЕ БУТЫЛКИ И ЯЧЕЙКА ДЛЯ ЛОПАТЫ.", "共 24 格，四瓶独立，另有铲子格。" },
  [kUi_QolDescShovel] = {"KEEPS THE SHOVEL AFTER FINDING THE FLUTE. RANDOMIZER OWNERSHIP IS RESPECTED.", "GARDE LA PELLE APRÈS LA FLÛTE. RESPECTE LES OBJETS ACQUIS EN RANDOMIZER.", "CONSERVA LA PALA AL CONSEGUIR LA FLAUTA. RESPETA LOS OBJETOS DEL RANDOMIZER.", "MANTÉM A PÁ APÓS OBTER A FLAUTA. RESPEITA OS ITENS DO RANDOMIZER.", "CONSERVA LA PALA DOPO IL FLAUTO. RISPETTA GLI OGGETTI DEL RANDOMIZER.", "BEHÄLT DIE SCHAUFEL NACH DER FLÖTE. BEACHTET DEN BESITZ IM RANDOMIZER.", "СОХРАНЯЕТ ЛОПАТУ ПОСЛЕ ПОЛУЧЕНИЯ ФЛЕЙТЫ. УЧИТЫВАЕТ ПРЕДМЕТЫ RANDOMIZER.", "拿到笛子后保留铲子，随机化归属照常。" },
  [kUi_QolDescMirror] = {"ALLOWS THE MAGIC MIRROR TO TRAVEL IN BOTH DIRECTIONS BETWEEN WORLDS.", "PERMET AU MIROIR DE PASSER ENTRE LES MONDES DANS LES DEUX SENS.", "PERMITE USAR EL ESPEJO MÁGICO EN AMBOS SENTIDOS ENTRE LOS MUNDOS.", "PERMITE USAR O ESPELHO MÁGICO NOS DOIS SENTIDOS ENTRE OS MUNDOS.", "PERMETTE DI USARE LO SPECCHIO MAGICO IN ENTRAMBE LE DIREZIONI TRA I MONDI.", "ERLAUBT DEM MAGISCHEN SPIEGEL DEN WELTENWECHSEL IN BEIDE RICHTUNGEN.", "ПОЗВОЛЯЕТ ВОЛШЕБНОМУ ЗЕРКАЛУ ПЕРЕМЕЩАТЬ МЕЖДУ МИРАМИ В ОБЕ СТОРОНЫ.", "魔法镜可在两个世界间双向往返。" },
  [kUi_Select] = {"SELECT", "CHOISIR", "ELEGIR", "ESCOLHER", "SCEGLI", "WÄHLEN", "ВЫБРАТЬ", "选择" },
  [kUi_SelectHelp] = {"A / START: SELECT", "A / START : CHOISIR", "A / START: ELEGIR", "A / START: ESCOLHER", "A / START: SCEGLI", "A / START: WÄHLEN", "A / START: ВЫБРАТЬ", "A / START: 选择" },
  [kUi_Test] = {"TEST", "TEST", "PRUEBA", "TESTE", "TEST", "TEST", "ТЕСТ", "测试" },
  [kUi_Magic] = {"MAGIC", "MAGIE", "MAGIA", "MAGIA", "MAGIA", "MAGIE", "МАГИЯ", "魔力" },
  [kUi_Life] = {"LIFE", "VIE", "VIDA", "VIDA", "VITA", "LEBEN", "ЖИЗНЬ", "生命" },
  [kUi_MainMenu] = {"MAIN MENU", "MENU PRINCIPAL", "MENÚ INICIAL", "MENU INICIAL", "MENU INIZIALE", "STARTMENÜ", "ГЛАВНОЕ МЕНЮ", "主菜单" },
  [kUi_ReturnMainMenu] = {"RETURN TO THE MAIN MENU?", "REVENIR AU MENU PRINCIPAL?", "VOLVER AL MENÚ INICIAL?", "VOLTAR AO MENU INICIAL?", "TORNARE AL MENU INIZIALE?", "ZUM STARTMENÜ ZURÜCKKEHREN?", "ВЕРНУТЬСЯ В ГЛАВНОЕ МЕНЮ?", "返回主菜单？" },
  [kUi_LanguageDescription] = {"GAME AND MENUS WILL USE ENGLISH. CONFIRM TO APPLY.", "LE JEU ET LES MENUS SERONT EN FRANÇAIS. CONFIRMEZ POUR APPLIQUER.", "EL JUEGO Y LOS MENÚS ESTARÁN EN ESPAÑOL. CONFIRMA PARA APLICARLO.", "O JOGO E OS MENUS FICARÃO EM PORTUGUÊS. CONFIRME PARA APLICAR.", "IL GIOCO E I MENU SARANNO IN ITALIANO. CONFERMA PER APPLICARE.", "SPIEL UND MENÜS WERDEN AUF DEUTSCH ANGEZEIGT. BITTE BESTÄTIGEN.", "ИГРА И МЕНЮ БУДУТ НА РУССКОМ ЯЗЫКЕ. ПОДТВЕРДИТЕ ВЫБОР.", "游戏与菜单将使用简体中文，确认后生效。" },
  [kUi_QolTitle] = {"QUALITY OF LIFE", "CONFORT DE JEU", "CALIDAD DE VIDA", "QUALIDADE DE VIDA", "MIGLIORIE", "KOMFORT", "УДОБСТВО ИГРЫ", "金手指" },
  [kUi_QolFastSwitch] = {"L/R ITEM SWITCH", "OBJETS AVEC L/R", "OBJETOS CON L/R", "ITENS COM L/R", "OGGETTI CON L/R", "L/R GEGENSTÄNDE", "СМЕНА ПРЕДМЕТОВ L/R", "L/R 切换道具" },
  [kUi_QolPegasus] = {"IMPROVED PEGASUS BOOTS", "BOTTES AMÉLIORÉES", "BOTAS MEJORADAS", "BOTAS MELHORADAS", "STIVALI MIGLIORATI", "BESSERE PEGASUSSTIEFEL", "УЛУЧШЕННЫЕ САПОГИ", "改良飞马靴" },
  [kUi_QolKakariko] = {"KAKARIKO SHORTCUT", "RACCOURCI COCORICO", "ATAJO A KAKARIKO", "ATALHO A KAKARIKO", "SCORCIATOIA KAKARIKO", "ABKÜRZUNG KAKARIKO", "КОРОТКИЙ ПУТЬ КАКАРИКО", "卡卡利科捷径" },
  [kUi_QolDimFlashes] = {"REDUCED FLASHES", "MOINS DE FLASHES", "MENOS DESTELLOS", "MENOS CLARÕES", "MENO LAMPEGGI", "WENIGER BLITZE", "МЕНЬШЕ ВСПЫШЕК", "减弱闪光" },
  [kUi_QolFairyHearts] = {"FAIRIES: 8 HEARTS", "FÉES : 8 COEURS", "HADAS: 8 CORAZONES", "FADAS: 8 CORAÇÕES", "FATE: 8 CUORI", "FEEN: 8 HERZEN", "ФЕИ: 8 СЕРДЕЦ", "妖精: 8 颗心" },
  [kUi_QolSwordPickup] = {"SWORD ITEM PICKUP", "RAMASSER AVEC ÉPÉE", "RECOGER CON ESPADA", "RECOLHER COM ESPADA", "RACCOGLI CON SPADA", "SAMMELN MIT SCHWERT", "СБОР ПРЕДМЕТОВ МЕЧОМ", "挥剑拾取道具" },
  [kUi_QolBrightRooms] = {"BRIGHT DARK ROOMS", "ÉCLAIRER LES SALLES", "ILUMINAR SALAS OSCURAS", "ILUMINAR SALAS ESCURAS", "ILLUMINA STANZE BUIE", "DUNKLE RÄUME ERHELLEN", "ОСВЕЩАТЬ ТЁМНЫЕ КОМНАТЫ", "照亮黑暗房间" },
  [kUi_QolMuteBeep] = {"MUTE LOW HEALTH BEEP", "COUPER ALARME DE VIE", "QUITAR PITIDO DE VIDA", "SEM ALERTA DE VIDA", "SILENZIA ALLARME VITA", "KEIN WARNTON BEI NOT", "БЕЗ СИГНАЛА МАЛОГО HP", "关闭低血提示音" },
  [kUi_QolRepeatBlocks] = {"REPEATED BLOCK PUSH", "REPOUSSER LES BLOCS", "MOVER BLOQUES DE NUEVO", "MOVER BLOCOS DE NOVO", "SPOSTA ANCORA I BLOCCHI", "BLÖCKE MEHRFACH SCHIEBEN", "ДВИГАТЬ БЛОКИ ПОВТОРНО", "方块可重复推" },
  [kUi_QolBombs99] = {"99 BOMBS", "99 BOMBES", "99 BOMBAS", "99 BOMBAS", "99 BOMBE", "99 BOMBEN", "99 БОМБ", "炸弹上限 99" },
  [kUi_QolArrows99] = {"99 ARROWS", "99 FLÈCHES", "99 FLECHAS", "99 FLECHAS", "99 FRECCE", "99 PFEILE", "99 СТРЕЛ", "箭上限 99" },
  [kUi_QolWallet9999] = {"WALLET: 9999 RUPEES", "BOURSE : 9999 RUBIS", "CARTERA: 9999 RUPIAS", "CARTEIRA: 9999 RÚPIAS", "BORSA: 9999 RUPIE", "GELDBEUTEL: 9999 RUBINE", "КОШЕЛЁК: 9999 РУПИЙ", "钱包: 9999 卢比" },
  [kUi_QolInventory] = {"EXPANDED INVENTORY", "INVENTAIRE ÉTENDU", "INVENTARIO AMPLIADO", "INVENTÁRIO AMPLIADO", "INVENTARIO AMPLIATO", "ERWEITERTES INVENTAR", "РАСШИРЕННЫЙ ИНВЕНТАРЬ", "扩展道具栏" },
  [kUi_QolShovel] = {"PERMANENT SHOVEL", "PELLE PERMANENTE", "PALA PERMANENTE", "PÁ PERMANENTE", "PALA PERMANENTE", "SCHAUFEL BEHALTEN", "СОХРАНЯТЬ ЛОПАТУ", "永久铲子" },
  [kUi_QolMirror] = {"MIRROR: BOTH WORLDS", "MIROIR : DEUX MONDES", "ESPEJO: AMBOS MUNDOS", "ESPELHO: DOIS MUNDOS", "SPECCHIO: DUE MONDI", "SPIEGEL: BEIDE WELTEN", "ЗЕРКАЛО: ОБА МИРА", "镜子: 两个世界" },
  [kUi_QolLive] = {"APPLIES WHILE PLAYING", "EFFET IMMÉDIAT", "CAMBIO INMEDIATO", "EFEITO IMEDIATO", "EFFETTO IMMEDIATO", "SOFORT ANWENDBAR", "ПРИМЕНЯЕТСЯ СРАЗУ", "游戏中即时生效" },
  [kUi_QolRestart] = {"RESTART REQUIRED", "REDÉMARRAGE REQUIS", "REQUIERE REINICIAR", "REQUER REINICIAR", "RIAVVIO NECESSARIO", "NEUSTART ERFORDERLICH", "ТРЕБУЕТСЯ ПЕРЕЗАПУСК", "需要重启" },
  [kUi_QolRestartQuestion] = {"RESTART THE GAME?", "REDÉMARRER LE JEU ?", "REINICIAR EL JUEGO?", "REINICIAR O JOGO?", "RIAVVIARE IL GIOCO?", "SPIEL NEU STARTEN?", "ПЕРЕЗАПУСТИТЬ ИГРУ?", "重启游戏？" },
  [kUi_QolExcessWarning] = {"AMMO OR RUPEES ABOVE ORIGINAL LIMITS WILL BE LOST.", "LES MUNITIONS OU RUBIS AU-DELÀ DES LIMITES INITIALES SERONT PERDUS.", "SE PERDERÁN LAS BOMBAS, FLECHAS O RUPIAS QUE SUPEREN EL LÍMITE ORIGINAL.", "BOMBAS, FLECHAS OU RÚPIAS ACIMA DO LIMITE ORIGINAL SERÃO PERDIDAS.", "BOMBE, FRECCE O RUPIE OLTRE I LIMITI ORIGINALI ANDRANNO PERSE.", "BOMBEN, PFEILE ODER RUBINE ÜBER DEM ORIGINALEN LIMIT GEHEN VERLOREN.", "БОМБЫ, СТРЕЛЫ ИЛИ РУПИИ СВЕРХ ИСХОДНОГО ЛИМИТА БУДУТ ПОТЕРЯНЫ.", "超过原始上限的弹药或卢比将会丢失。" },
  [kUi_QolHelp] = {"A: CHANGE   B: BACK", "A : CHANGER   B : RETOUR", "A: CAMBIAR   B: VOLVER", "A: ALTERAR   B: VOLTAR", "A: CAMBIA   B: INDIETRO", "A: ÄNDERN   B: ZURÜCK", "A: ИЗМЕНИТЬ   B: НАЗАД", "A: 修改   B: 返回" },
  [kUi_DeveloperTool] = {"DEVELOPER TOOL", "OUTIL DE DÉVELOPPEMENT", "HERRAMIENTA DE DESARROLLADOR", "FERRAMENTA DE DESENVOLVEDOR", "STRUMENTO PER SVILUPPATORI", "ENTWICKLERWERKZEUG", "ИНСТРУМЕНТ РАЗРАБОТЧИКА", "开发者工具" },
  [kUi_Start] = {"START", "DÉMARRER", "INICIAR", "INICIAR", "AVVIA", "STARTEN", "НАЧАТЬ", "开始" },
  [kUi_Change] = {"CHANGE", "CHANGER", "CAMBIAR", "ALTERAR", "CAMBIA", "ÄNDERN", "ИЗМЕНИТЬ", "修改" },
  [kUi_NoMods] = {"NO MODS", "AUCUN MOD", "SIN MODS", "SEM MODS", "NESSUNA MOD", "KEINE MODS", "НЕТ МОДОВ", "无模组" },
  [kUi_Skins] = {"SKINS", "APPARENCES", "ASPECTOS", "VISUAIS", "ASPETTI", "SKINS", "ОБЛИКИ", "皮肤" },
  [kUi_Continue] = {"CONTINUE", "CONTINUER", "CONTINUAR", "CONTINUAR", "CONTINUA", "WEITER", "ПРОДОЛЖИТЬ", "继续" },
  [kUi_Quit] = {"QUIT", "QUITTER", "SALIR", "SAIR", "ESCI", "BEENDEN", "ВЫХОД", "退出" },
  [kUi_Mods] = {"MODS", "MODS", "MODS", "MODS", "MOD", "MODS", "МОДЫ", "模组" },
  [kUi_Loading] = {"LOADING", "CHARGEMENT", "CARGANDO", "CARREGANDO", "CARICAMENTO", "LADEN", "ЗАГРУЗКА", "载入中" },
  [kUi_RandoInvalidFile] = {"THIS JSON FILE IS INVALID OR TOO LARGE.", "CE FICHIER JSON EST INVALIDE OU TROP GRAND.", "EL JSON NO ES VÁLIDO O ES DEMASIADO GRANDE.", "O JSON É INVÁLIDO OU MUITO GRANDE.", "IL FILE JSON NON È VALIDO O È TROPPO GRANDE.", "DIE JSON-DATEI IST UNGÜLTIG ODER ZU GROSS.", "ФАЙЛ JSON ПОВРЕЖДЁН ИЛИ СЛИШКОМ БОЛЬШОЙ.", "该 JSON 文件无效或过大。" },
  [kUi_RandoUnsupported] = {"THIS COMBINATION OF SEED RULES IS NOT AVAILABLE YET.", "CETTE COMBINAISON DE RÈGLES N'EST PAS ENCORE DISPONIBLE.", "ESTA COMBINACIÓN DE REGLAS AÚN NO ESTÁ DISPONIBLE.", "ESTA COMBINAÇÃO DE REGRAS AINDA NÃO ESTÁ DISPONÍVEL.", "QUESTA COMBINAZIONE DI REGOLE NON È ANCORA DISPONIBILE.", "DIESE KOMBINATION VON REGELN IST NOCH NICHT VERFÜGBAR.", "ЭТО СОЧЕТАНИЕ ПРАВИЛ ПОКА НЕ ПОДДЕРЖИВАЕТСЯ.", "此种子规则组合暂不可用。" },
  [kUi_RandoMissingData] = {"EXACT PLACEMENTS ARE MISSING. EXPORT THE FULL SPOILER JSON FROM ALttPR.", "PLACEMENTS MANQUANTS. EXPORTEZ LE JSON COMPLET DU SPOILER ALttPR.", "FALTAN LAS UBICACIONES DE LOS PREMIOS. EXPORTA EL JSON COMPLETO DE ALttPR.", "FALTAM AS POSIÇÕES DOS ITENS. EXPORTE O JSON COMPLETO DO ALttPR.", "MANCANO LE POSIZIONI DEGLI OGGETTI. ESPORTA IL JSON COMPLETO DI ALttPR.", "GEGENSTANDSORTE FEHLEN. VOLLSTÄNDIGEN ALttPR-SPOILER ALS JSON EXPORTIEREN.", "НЕТ РАСПОЛОЖЕНИЯ ПРЕДМЕТОВ. ЭКСПОРТИРУЙТЕ ПОЛНЫЙ JSON СПОЙЛЕРА ALttPR.", "缺少道具放置数据，请导出 ALttPR 完整剧透 JSON。" },
  [kUi_RandoUncertified] = {"THIS EXACT SEED COULD NOT BE VERIFIED. NO SAVE WAS CREATED.", "CETTE GRAINE N'A PAS PU ÊTRE VÉRIFIÉE. AUCUNE SAUVEGARDE CRÉÉE.", "NO SE PUDO VERIFICAR ESTA SEMILLA. NO SE CREÓ NINGUNA PARTIDA.", "NÃO FOI POSSÍVEL VERIFICAR A SEMENTE. NENHUM JOGO FOI CRIADO.", "IMPOSSIBILE VERIFICARE QUESTO SEME. NESSUN SALVATAGGIO CREATO.", "DIESER SEED KONNTE NICHT GEPRÜFT WERDEN. KEIN SPIELSTAND ERSTELLT.", "НЕ УДАЛОСЬ ПРОВЕРИТЬ ЭТОТ СИД. СОХРАНЕНИЕ НЕ СОЗДАНО.", "无法校验此种子，未创建存档。" },
  [kUi_RandoStorageError] = {"UNABLE TO READ OR SAVE THIS SEED. PREVIOUS FILES WERE KEPT.", "LECTURE OU ÉCRITURE IMPOSSIBLE. LES FICHIERS PRÉCÉDENTS SONT CONSERVÉS.", "NO SE PUDO LEER O GUARDAR LA SEMILLA. SE CONSERVARON LOS ARCHIVOS ANTERIORES.", "NÃO FOI POSSÍVEL LER OU SALVAR. OS ARQUIVOS ANTERIORES FORAM MANTIDOS.", "IMPOSSIBILE LEGGERE O SALVARE. I FILE PRECEDENTI SONO STATI CONSERVATI.", "SEED KANN NICHT GELESEN ODER GESPEICHERT WERDEN. ALTE DATEIEN BLEIBEN ERHALTEN.", "НЕ УДАЛОСЬ ПРОЧИТАТЬ ИЛИ ЗАПИСАТЬ СИД. ПРЕЖНИЕ ФАЙЛЫ СОХРАНЕНЫ.", "无法读取或保存此种子，原文件已保留。" },
  [kUi_RandoSourceChanged] = {"THE IMPORTED JSON IS MISSING OR CHANGED. RESTORE ITS ORIGINAL COPY.", "LE JSON IMPORTÉ MANQUE OU A CHANGÉ. RESTAUREZ SA COPIE D'ORIGINE.", "FALTA EL JSON IMPORTADO O HA CAMBIADO. RESTAURA SU COPIA ORIGINAL.", "O JSON IMPORTADO SUMIU OU MUDOU. RESTAURE A CÓPIA ORIGINAL.", "IL JSON IMPORTATO MANCA O È CAMBIATO. RIPRISTINA LA COPIA ORIGINALE.", "IMPORTIERTE JSON-DATEI FEHLT ODER WURDE GEÄNDERT. ORIGINAL WIEDERHERSTELLEN.", "ИМПОРТИРОВАННЫЙ JSON ИЗМЕНЁН ИЛИ ОТСУТСТВУЕТ. ВЕРНИТЕ ИСХОДНУЮ КОПИЮ.", "导入的 JSON 丢失或已改动，请恢复原始文件。" },
  [kUi_SaveState] = {"SAVE STATE", "SAUVER ÉTAT", "GUARDAR ESTADO", "SALVAR ESTADO", "SALVA STATO", "ZUSTAND SICHERN", "СОХРАНИТЬ", "保存状态" },
  [kUi_LoadDiagnostic] = {"LOAD DUMP", "CHARGER DUMP", "CARGAR DUMP", "CARREGAR DUMP", "CARICA DUMP", "DUMP LADEN", "ЗАГРУЗИТЬ ДАМП", "载入转储" },
  [kUi_LoadSavedState] = {"LOAD THE SAVED STATE", "CHARGER L'ÉTAT SAUVÉ", "CARGAR EL ESTADO GUARDADO", "CARREGAR O ESTADO SALVO", "CARICA LO STATO SALVATO", "GESICHERTEN ZUSTAND LADEN", "ЗАГРУЗИТЬ СОХРАНЕНИЕ", "载入已存状态" },
  [kUi_Tests] = {"TESTS", "TESTS", "PRUEBAS", "TESTES", "TEST", "TESTS", "ТЕСТЫ", "测试" },
  [kUi_TestsHelp] = {"A: LOAD   B: BACK   L/R: PAGE", "A : CHARGER   B : RETOUR", "A: CARGAR   B: VOLVER   L/R: PÁGINA", "A: CARREGAR   B: VOLTAR", "A: CARICA   B: INDIETRO", "A: LADEN   B: ZURÜCK", "A: ЗАГРУЗИТЬ   B: НАЗАД", "A: 载入   B: 返回   L/R: 翻页" },
  [kUi_ScenarioMissing] = {"NO STATE FILE IN THIS BUILD", "FICHIER D'ÉTAT ABSENT", "ESTA VERSIÓN NO TIENE EL ESTADO", "ESTADO AUSENTE NESTA VERSÃO", "STATO ASSENTE IN QUESTA VERSIONE", "ZUSTAND FEHLT IN DIESER VERSION", "НЕТ ФАЙЛА СОСТОЯНИЯ", "此版本无状态文件" },
  [kUi_CurrentSeed] = {"SEED IN PLAY", "GRAINE EN COURS", "SEMILLA ACTUAL", "SEMENTE ATUAL", "SEME IN CORSO", "AKTUELLER SEED", "ТЕКУЩИЙ СИД", "当前种子" },
  [kUi_LeaveRandomizer] = {"RETURN TO THE NORMAL GAME?", "REVENIR AU JEU NORMAL?", "VOLVER AL JUEGO NORMAL?", "VOLTAR AO JOGO NORMAL?", "TORNARE AL GIOCO NORMALE?", "ZUM NORMALEN SPIEL?", "ВЕРНУТЬСЯ К ОБЫЧНОЙ ИГРЕ?", "返回普通游戏？" },
  [kUi_ImportSeed] = {"IMPORT", "IMPORTER", "IMPORTAR", "IMPORTAR", "IMPORTA", "IMPORT", "ИМПОРТ", "导入" },
  [kUi_ImportHelp] = {"COPY AN ALttPR SPOILER JSON HERE:", "COPIEZ UN JSON ALttPR ICI :", "COPIA UN JSON DE ALttPR AQUÍ:", "COPIE UM JSON DE ALttPR AQUI:", "COPIA UN JSON ALttPR QUI:", "ALttPR-SPOILER-JSON HIER ABLEGEN:", "СКОПИРУЙТЕ JSON ALttPR СЮДА:", "将 ALttPR 剧透 JSON 复制到此处:" },
  [kUi_SeedList] = {"SEEDS", "GRAINES", "SEMILLAS", "SEMENTES", "SEMI", "SEEDS", "СИДЫ", "种子" },
  [kUi_Randomizer] = {"RANDOMIZER", "RANDOMIZER", "RANDOMIZER", "RANDOMIZER", "RANDOMIZER", "RANDOMIZER", "РАНДОМАЙЗЕР", "随机化" },
  [kUi_OpenRandomizer] = {"START THIS SEED?", "LANCER CETTE GRAINE ?", "INICIAR ESTA SEMILLA?", "INICIAR ESTA SEMENTE?", "AVVIARE QUESTO SEME?", "DIESEN SEED STARTEN?", "ЗАПУСТИТЬ ЭТОТ СИД?", "开始此种子？" },
  [kUi_Seed] = {"SEED", "GRAINE", "SEMILLA", "SEMENTE", "SEME", "SEED", "СИД", "种子" },
  [kUi_Preset] = {"MODE", "MODE", "MODO", "MODO", "MODALITÀ", "MODUS", "РЕЖИМ", "模式" },
  [kUi_GenerateSeed] = {"GENERATE", "GÉNÉRER", "GENERAR", "GERAR", "GENERA", "ERSTELLEN", "СОЗДАТЬ", "生成" },
  [kUi_SavedSeeds] = {"SAVED SEEDS", "GRAINES SAUVÉES", "SEMILLAS GUARDADAS", "SEMENTES SALVAS", "SEMI SALVATI", "GESPEICHERTE SEEDS", "СОХРАНЁННЫЕ СИДЫ", "已存种子" },
  [kUi_OrdinaryGame] = {"NORMAL GAME", "JEU NORMAL", "JUEGO NORMAL", "JOGO NORMAL", "GIOCO NORMALE", "NORMALES SPIEL", "ОБЫЧНАЯ ИГРА", "普通游戏" },
  [kUi_NoSeeds] = {"NO SAVED SEEDS", "AUCUNE GRAINE SAUVÉE", "NO HAY SEMILLAS GUARDADAS", "NENHUMA SEMENTE SALVA", "NESSUN SEME SALVATO", "KEINE GESPEICHERTEN SEEDS", "НЕТ СОХРАНЁННЫХ СИДОВ", "无已存种子" },
  [kUi_SkinsOnly] = {"GAME TEXT: ENGLISH. SKINS ONLY. NORMAL SAVES STAY SEPARATE.", "TEXTES DU JEU EN ANGLAIS. SKINS SEULS. SAUVEGARDES NORMALES SÉPARÉES.", "TEXTO EN INGLÉS. SOLO SKINS. PARTIDAS NORMALES SEPARADAS.", "TEXTO EM INGLÊS. SÓ SKINS. PARTIDAS NORMAIS SEPARADAS.", "TESTO IN INGLESE. SOLO SKIN. SALVATAGGI NORMALI SEPARATI.", "SPIELTEXT AUF ENGLISCH. NUR SKINS. NORMALE SPIELSTÄNDE GETRENNT.", "ТЕКСТ ИГРЫ НА АНГЛИЙСКОМ. ТОЛЬКО СКИНЫ. ОБЫЧНЫЕ СОХРАНЕНИЯ ОТДЕЛЬНО.", "游戏文本为英文，仅皮肤，普通存档独立。" },
  [kUi_RandoOpen] = {"OPEN", "OUVERT", "ABIERTO", "ABERTO", "APERTO", "OFFEN", "ОТКРЫТЫЙ", "开放" },
  [kUi_RandoStandard] = {"STANDARD", "STANDARD", "ESTÁNDAR", "PADRÃO", "STANDARD", "STANDARD", "СТАНДАРТНЫЙ", "标准" },
  [kUi_RandoInverted] = {"INVERTED", "INVERSÉ", "INVERTIDO", "INVERTIDO", "INVERTITO", "INVERTIERT", "ИНВЕРСИЯ", "反转" },
  [kUi_RandoRetro] = {"RETRO", "RÉTRO", "RETRO", "RETRÔ", "RETRO", "RETRO", "РЕТРО", "复古" },
  [kUi_RandoTriforce] = {"TRIFORCE HUNT", "CHASSE TRIFORCE", "BUSCAR TRIFUERZA", "CAÇA À TRIFORCE", "TRIFORZA", "TRIFORCE-JAGD", "ТРИФОРС", "三角力量" },
  [kUi_Delete] = {"DELETE", "EFFACER", "BORRAR", "APAGAR", "CANCELLA", "LÖSCHEN", "УДАЛИТЬ", "删除" },
  [kUi_ChangeGameLanguage] = {"CHANGE GAME LANGUAGE?", "CHANGER LA LANGUE DU JEU?", "CAMBIAR EL IDIOMA DEL JUEGO?", "MUDAR O IDIOMA DO JOGO?", "CAMBIARE LINGUA DEL GIOCO?", "SPIELSPRACHE ÄNDERN?", "СМЕНИТЬ ЯЗЫК ИГРЫ?", "更改游戏语言？" },
  [kUi_No] = {"NO", "NON", "NO", "NÃO", "NO", "NEIN", "НЕТ", "否" },
  [kUi_Yes] = {"YES", "OUI", "SÍ", "SIM", "SÌ", "JA", "ДА", "是" },
  [kUi_ErrorSdWrite] = {"SD CARD WRITE FAILED", "ÉCRITURE SD IMPOSSIBLE", "ERROR AL ESCRIBIR EN SD", "ERRO AO GRAVAR NO SD", "ERRORE SCRITTURA SD", "SD-SCHREIBFEHLER", "ОШИБКА ЗАПИСИ НА SD", "SD 卡写入失败" },
  [kUi_ErrorWifi] = {"NO WI-FI CONNECTION", "AUCUNE CONNEXION WI-FI", "SIN CONEXIÓN WI-FI", "SEM CONEXÃO WI-FI", "NESSUNA CONNESSIONE WI-FI", "KEINE WLAN-VERBINDUNG", "НЕТ СОЕДИНЕНИЯ WI-FI", "未连接 WI-FI" },
  [kUi_ErrorTls] = {"TLS SERVICE FAILED", "ÉCHEC DU SERVICE TLS", "ERROR DEL SERVICIO TLS", "ERRO NO SERVIÇO TLS", "ERRORE SERVIZIO TLS", "TLS-DIENST FEHLGESCHLAGEN", "ОШИБКА СЛУЖБЫ TLS", "TLS 服务失败" },
  [kUi_ErrorRelease] = {"INVALID RELEASE DATA", "DONNÉES DE VERSION INVALIDES", "DATOS DE VERSIÓN INVÁLIDOS", "DADOS DE VERSÃO INVÁLIDOS", "DATI VERSIONE NON VALIDI", "UNGÜLTIGE VERSIONSDATEN", "НЕВЕРНЫЕ ДАННЫЕ ВЕРСИИ", "版本数据无效" },
  [kUi_ErrorSpace] = {"NOT ENOUGH SD SPACE", "ESPACE SD INSUFFISANT", "ESPACIO INSUFICIENTE EN SD", "ESPAÇO INSUFICIENTE NO SD", "SPAZIO SD INSUFFICIENTE", "ZU WENIG SD-SPEICHER", "НЕДОСТАТОЧНО МЕСТА НА SD", "SD 卡空间不足" },
  [kUi_ErrorLaunchPath] = {"3DSX LAUNCH PATH UNKNOWN", "CHEMIN DU 3DSX INCONNU", "RUTA DEL 3DSX DESCONOCIDA", "CAMINHO DO 3DSX DESCONHECIDO", "PERCORSO 3DSX SCONOSCIUTO", "3DSX-STARTPFAD UNBEKANNT", "ПУТЬ К 3DSX НЕИЗВЕСТЕН", "3DSX 启动路径未知" },
  [kUi_ErrorDownload] = {"DOWNLOAD CHECK FAILED", "TÉLÉCHARGEMENT NON VALIDE", "DESCARGA NO VÁLIDA", "DOWNLOAD INVÁLIDO", "DOWNLOAD NON VALIDO", "DOWNLOAD-PRÜFUNG FEHLGESCHLAGEN", "ОШИБКА ПРОВЕРКИ ЗАГРУЗКИ", "下载校验失败" },
  [kUi_ErrorInstall] = {"INSTALLATION FAILED", "ÉCHEC DE L'INSTALLATION", "ERROR DE INSTALACIÓN", "ERRO DE INSTALAÇÃO", "INSTALLAZIONE FALLITA", "INSTALLATION FEHLGESCHLAGEN", "ОШИБКА УСТАНОВКИ", "安装失败" },
  [kUi_ErrorCancelled] = {"UPDATE CANCELLED", "MISE À JOUR ANNULÉE", "ACTUALIZACIÓN CANCELADA", "ATUALIZAÇÃO CANCELADA", "AGGIORNAMENTO ANNULLATO", "UPDATE ABGEBROCHEN", "ОБНОВЛЕНИЕ ОТМЕНЕНО", "更新已取消" },
  [kUi_ErrorConnection] = {"CONNECTION FAILED - RETRY", "CONNEXION IMPOSSIBLE - RÉESSAYER", "FALLO DE CONEXIÓN - REINTENTA", "FALHA DE CONEXÃO - TENTE DE NOVO", "CONNESSIONE FALLITA - RIPROVA", "VERBINDUNGSFEHLER - ERNEUT VERSUCHEN", "НЕТ СВЯЗИ - ПОВТОРИТЕ", "连接失败 - 重试" },
  [kUi_ErrorStart] = {"CANNOT START UPDATE", "DÉMARRAGE IMPOSSIBLE", "NO SE PUEDE INICIAR", "NÃO FOI POSSÍVEL INICIAR", "IMPOSSIBILE AVVIARE", "UPDATE-START FEHLGESCHLAGEN", "НЕ УДАЛОСЬ НАЧАТЬ", "无法开始更新" },
  [kUi_ErrorChannel] = {"CANNOT SAVE UPDATE CHANNEL", "ENREGISTREMENT DU CANAL IMPOSSIBLE", "NO SE PUEDE GUARDAR EL CANAL", "NÃO FOI POSSÍVEL SALVAR O CANAL", "IMPOSSIBILE SALVARE IL CANALE", "KANAL KANN NICHT GESPEICHERT WERDEN", "НЕ УДАЛОСЬ СОХРАНИТЬ КАНАЛ", "无法保存更新通道" },
  [kUi_Changes] = {"CHANGES", "NOUVEAUTÉS", "CAMBIOS", "NOVIDADES", "NOVITÀ", "NEUERUNGEN", "ИЗМЕНЕНИЯ", "更新内容" },
  [kUi_Page] = {"PAGE", "PAGE", "PÁGINA", "PÁGINA", "PAGINA", "SEITE", "СТРАНИЦА", "页" },
  [kUi_XHold] = {"X HOLD", "X MAINT.", "MANT. X", "SEGURAR X", "TIENI X", "X HALTEN", "ДЕРЖАТЬ X", "按住 X" },
  [kUi_Checking] = {"CHECKING FOR UPDATES", "RECHERCHE EN COURS", "BUSCANDO ACTUALIZACIÓN", "BUSCANDO ATUALIZAÇÃO", "RICERCA AGGIORNAMENTI", "SUCHE NACH UPDATES", "ПОИСК ОБНОВЛЕНИЙ", "正在检查更新" },
  [kUi_UpToDate] = {"YOU ARE UP TO DATE", "VERSION À JOUR", "ESTÁ ACTUALIZADO", "ESTÁ ATUALIZADO", "VERSIONE AGGIORNATA", "AUF DEM NEUESTEN STAND", "ВЕРСИЯ АКТУАЛЬНА", "已是最新版本" },
  [kUi_UpdateReady] = {"UPDATE AVAILABLE", "MISE À JOUR DISPONIBLE", "ACTUALIZACIÓN DISPONIBLE", "ATUALIZAÇÃO DISPONÍVEL", "AGGIORNAMENTO DISPONIBILE", "UPDATE VERFÜGBAR", "ЕСТЬ ОБНОВЛЕНИЕ", "有可用更新" },
  [kUi_NoUpdate] = {"NO UPDATE AVAILABLE", "AUCUNE MISE À JOUR", "SIN ACTUALIZACIONES", "SEM ATUALIZAÇÕES", "NESSUN AGGIORNAMENTO", "KEIN UPDATE VERFÜGBAR", "НЕТ ОБНОВЛЕНИЙ", "没有可用更新" },
  [kUi_Downloading] = {"DOWNLOADING", "TÉLÉCHARGEMENT", "DESCARGANDO", "BAIXANDO", "SCARICAMENTO", "HERUNTERLADEN", "ЗАГРУЗКА", "正在下载" },
  [kUi_Verifying] = {"VERIFYING DOWNLOAD", "VÉRIFICATION", "VERIFICANDO DESCARGA", "VERIFICANDO DOWNLOAD", "VERIFICA DOWNLOAD", "DOWNLOAD WIRD GEPRÜFT", "ПРОВЕРКА ЗАГРУЗКИ", "正在校验下载" },
  [kUi_Installing] = {"INSTALLING UPDATE", "INSTALLATION EN COURS", "INSTALANDO ACTUALIZACIÓN", "INSTALANDO ATUALIZAÇÃO", "INSTALLAZIONE IN CORSO", "UPDATE WIRD INSTALLIERT", "УСТАНОВКА ОБНОВЛЕНИЯ", "正在安装更新" },
  [kUi_UpdateError] = {"UPDATE FAILED", "ÉCHEC DE LA MISE À JOUR", "ERROR AL ACTUALIZAR", "ERRO AO ATUALIZAR", "AGGIORNAMENTO FALLITO", "UPDATE FEHLGESCHLAGEN", "ОШИБКА ОБНОВЛЕНИЯ", "更新失败" },
  [kUi_StateLoaded] = {"LOADED", "CHARGÉ", "CARGADO", "CARREGADO", "CARICATO", "GELADEN", "ЗАГРУЖЕНО", "已载入" },
  [kUi_NoDump] = {"NO DUMP", "AUCUN VIDAGE", "SIN COPIA", "SEM CÓPIA", "NESSUNA COPIA", "KEINE KOPIE", "НЕТ КОПИИ", "无转储" },
  [kUi_NoState] = {"NO STATE", "AUCUN ÉTAT", "SIN ESTADO", "SEM ESTADO", "NESSUNO STATO", "KEIN ZUSTAND", "НЕТ СОСТОЯНИЯ", "无状态" },
  [kUi_InvalidState] = {"INVALID", "INVALIDE", "INVÁLIDO", "INVÁLIDO", "NON VALIDO", "UNGÜLTIG", "НЕВЕРНЫЙ ФАЙЛ", "无效" },
  [kUi_WrongGame] = {"OTHER GAME", "AUTRE JEU", "OTRO JUEGO", "OUTRO JOGO", "ALTRO GIOCO", "ANDERES SPIEL", "ДРУГАЯ ИГРА", "其他游戏" },
  [kUi_IoError] = {"READ ERROR", "ERREUR LECTURE", "ERROR LECTURA", "ERRO LEITURA", "ERRORE LETTURA", "LESEFEHLER", "ОШИБКА ЧТЕНИЯ", "读取错误" },
  [kUi_Gameplay] = {"GAME", "JEU", "JUEGO", "JOGO", "GIOCO", "SPIEL", "ИГРА", "游戏" },
  [kUi_TurboControl] = {"TURBO BUTTON", "TOUCHE TURBO", "BOTÓN TURBO", "BOTÃO TURBO", "TASTO TURBO", "TURBOTASTE", "КНОПКА ТУРБО", "加速键" },
  [kUi_Autosave] = {"AUTO STATE", "ÉTAT AUTO", "ESTADO AUTO", "ESTADO AUTO", "STATO AUTO", "AUTO ZUSTAND", "АВТОСОСТ.", "自动状态" },
  [kUi_GameLanguage] = {"GAME LANGUAGE", "LANGUE DU JEU", "IDIOMA JUEGO", "IDIOMA DO JOGO", "LINGUA GIOCO", "SPIELSPRACHE", "ЯЗЫК ИГРЫ", "游戏语言" },
  [kUi_Gear]            = {"GEAR",          "\xC3\x89QUIP.", "EQUIPO", "EQUIP.", "STATO", "STATUS", "СТАТУС", "装备" },
  [kUi_Map]             = {"MAP",           "CARTE", "MAPA", "MAPA", "MAPPA", "KARTE", "КАРТА", "地图" },
  [kUi_Items]           = {"ITEMS",         "OBJETS", "OBJETOS", "ITENS", "OGGETTI", "ITEMS", "ВЕЩИ", "道具" },
  [kUi_Dungeon]         = {"DUNGEON",       "DONJON", "MAZMORRA", "MASMORRA", "DUNGEON", "VERLIES", "ПОДЗЕМЕЛЬЕ", "迷宫" },
  [kUi_Bottles]         = {"BOTTLES",       "FLACONS", "BOTELLAS", "GARRAFAS", "BOTTIGLIE", "FLASCHEN", "БУТЫЛКИ", "瓶子" },
  [kUi_Pendants]        = {"PENDANTS",      "PENDENTIFS", "COLGANTES", "PINGENTES", "PENDENTI", "AMULETTE", "КУЛОНЫ", "吊坠" },
  [kUi_Crystals]        = {"CRYSTALS",      "CRISTAUX", "CRISTALES", "CRISTAIS", "CRISTALLI", "KRISTALLE", "КРИСТАЛЛЫ", "水晶" },

  [kUi_Settings]        = {"SETTINGS",      "OPTIONS", "AJUSTES", "OPÇÕES", "OPZIONI", "OPTIONEN", "НАСТРОЙКИ", "设置" },
  [kUi_Screen]          = {"SCREEN",        "\xC3\x89""CRAN", "PANTALLA", "TELA", "SCHERMO", "BILD", "ЭКРАН", "画面" },
  [kUi_TurboSpeed]      = {"TURBO SPEED",   "VITESSE TURBO", "VELOCIDAD", "VELOCIDADE", "VELOCITÀ", "TEMPO", "СКОРОСТЬ", "加速倍率" },
  [kUi_Developer]       = {"DEVELOPER",     "D\xC3\x89V.", "DESARROLLO", "DESENVOLV.", "SVILUPPO", "ENTWICKLER", "ОТЛАДКА", "开发者选项" },
  [kUi_Update]          = {"UPDATE",        "MISE \xC3\x80 JOUR", "ACTUALIZAR", "ATUALIZAR", "AGGIORNA", "UPDATE", "ОБНОВЛЕНИЕ", "更新" },
  [kUi_Restart]         = {"RESTART",       "RED\xC3\x89MARRER", "REINICIAR", "REINICIAR", "RIAVVIA", "NEUSTART", "ПЕРЕЗАПУСК", "重启" },
  [kUi_Language]        = {"LANGUAGE",      "LANGUE", "IDIOMA", "IDIOMA", "LINGUA", "SPRACHE", "ЯЗЫК", "语言" },
  [kUi_Back]            = {"BACK",          "RETOUR", "VOLVER", "VOLTAR", "INDIETRO", "ZURÜCK", "НАЗАД", "返回" },
  [kUi_Off]             = {"OFF",           "NON", "NO", "NÃO", "NO", "AUS", "ВЫКЛ.", "关闭" },
  [kUi_On]              = {"ON",            "OUI", "SÍ", "SIM", "SÌ", "AN", "ВКЛ.", "开启" },
  [kUi_Open]            = {"OPEN",          "OUVRIR", "ABRIR", "ABRIR", "APRI", "ÖFFNEN", "ОТКРЫТЬ", "打开" },
  [kUi_Cancel]          = {"CANCEL",        "ANNULER", "CANCELAR", "CANCELAR", "ANNULLA", "ABBRUCH", "ОТМЕНА", "取消" },

  [kUi_DisplayMode]     = {"DISPLAY MODE",  "AFFICHAGE", "VISTA", "EXIBIÇÃO", "VISUALIZZA", "ANSICHT", "РЕЖИМ", "显示模式" },
  [kUi_EdgeMode]        = {"EDGE MODE",     "MODE BORDS", "CÁMARA", "CÂMERA", "CAMERA", "KAMERA", "КРАЯ", "边缘模式" },
  [kUi_Zoom]            = {"ZOOM",          "ZOOM", "ZOOM", "ZOOM", "ZOOM", "ZOOM", "МАСШТАБ", "缩放" },
  [kUi_TopHud]          = {"TOP HUD",       "HUD HAUT", "HUD SUPERIOR", "HUD SUPERIOR", "HUD IN ALTO", "OBERES HUD", "ВЕРХНИЙ HUD", "上屏 HUD" },
  [kUi_Wide]            = {"WIDE",          "LARGE", "AMPLIA", "AMPLA", "AMPIO", "BREIT", "ШИРОКИЙ", "宽屏" },
  [kUi_Original]        = {"ORIGINAL",      "ORIGINAL", "ORIGINAL", "ORIGINAL", "ORIGINALE", "ORIGINAL", "ИСХОДНЫЙ", "原始" },
  [kUi_Stretch]         = {"STRETCH",       "\xC3\x89TIR\xC3\x89", "ESTIRADA", "ESTICADA", "ESTESO", "GESTRECKT", "РАСТЯНУТЬ", "拉伸" },
  [kUi_FixedCamera]     = {"FIXED CAMERA",  "CAM\xC3\x89RA FIXE", "FIJA", "FIXA", "FISSA", "FEST", "ФИКС.", "固定" },
  [kUi_Standard]        = {"STANDARD",      "STANDARD", "NORMAL", "PADRÃO", "NORMALE", "STANDARD", "ОБЫЧНЫЙ", "标准" },

  [kUi_MemDump] = {"DUMP", "DUMP", "DUMP", "DUMP", "DUMP", "DUMP", "ДАМП", "转储" },
  [kUi_LoadState]       = {"LOAD STATE",    "CHARGER \xC3\x89TAT", "CARGAR ESTADO", "CARREGAR ESTADO", "CARICA STATO", "ZUSTAND LADEN", "ЗАГРУЗИТЬ", "载入状态" },
  [kUi_ShowFps]         = {"SHOW FPS",      "AFFICHER FPS", "MOSTRAR FPS", "MOSTRAR FPS", "MOSTRA FPS", "FPS ANZEIGEN", "ПОКАЗ FPS", "显示 FPS" },
  [kUi_Overlay]         = {"OVERLAY",       "INFOS", "INFORMACIÓN", "INFORMAÇÕES", "INFORMAZIONI", "INFORMATION", "СВЕДЕНИЯ", "叠加信息" },
  [kUi_Write]           = {"WRITE",         "\xC3\x89""CRIRE", "CREAR", "CRIAR", "CREA", "ERSTELLEN", "СОЗДАТЬ", "写入" },
  [kUi_Done]            = {"DONE",          "FAIT", "LISTO", "PRONTO", "FATTO", "FERTIG", "ГОТОВО", "完成" },
  [kUi_Wait]            = {"WAIT",          "ATTENTE", "ESPERA", "AGUARDE", "ATTENDI", "WARTEN", "ЖДИТЕ", "等待" },
  [kUi_Load]            = {"LOAD",          "CHARGER", "CARGAR", "CARREGAR", "CARICA", "LADEN", "ЗАГРУЗИТЬ", "载入" },
  [kUi_LoadDumpLine1] = {"LOAD LATEST STATE", "CHARGER LE DERNIER ÉTAT", "CARGAR ÚLTIMO ESTADO", "CARREGAR ÚLTIMO ESTADO", "CARICA ULTIMO STATO", "LETZTEN ZUSTAND LADEN", "ЗАГРУЗИТЬ СОСТОЯНИЕ", "载入最新状态" },
  [kUi_LoadDumpLine2] = {"FOR THIS GAME", "POUR CE JEU", "DE ESTE JUEGO", "DESTE JOGO", "DI QUESTO GIOCO", "FÜR DIESES SPIEL", "ДЛЯ ЭТОЙ ИГРЫ", "本游戏" },
  [kUi_ProgressLost]    = {"UNSAVED PROGRESS IS LOST", "PROGRESSION NON SAUV\xC3\x89""E PERDUE", "SE PIERDE LO NO GUARDADO", "PROGRESSO NÃO SALVO SERÁ PERDIDO", "PROGRESSI NON SALVATI PERSI", "UNGESPEICHERTES GEHT VERLOREN", "НЕСОХРАНЁННЫЙ ПРОГРЕСС БУДЕТ ПОТЕРЯН", "未保存的进度将丢失" },

  [kUi_Version]         = {"VERSION",       "VERSION", "VERSIÓN", "VERSÃO", "VERSIONE", "VERSION", "ВЕРСИЯ", "版本" },
  [kUi_Model]           = {"MODEL",         "MOD\xC3\x88LE", "MODELO", "MODELO", "MODELLO", "MODELL", "МОДЕЛЬ", "型号" },
  [kUi_FpsNow]          = {"FPS NOW",       "FPS ACT", "FPS ACTUAL", "FPS ATUAL", "FPS ATTUALI", "FPS AKTUELL", "ТЕКУЩИЙ FPS", "当前 FPS" },
  [kUi_FpsAvg]          = {"FPS AVG",       "FPS MOY", "FPS MEDIO", "FPS MÉDIO", "FPS MEDI", "FPS MITTEL", "СРЕДНИЙ FPS", "平均 FPS" },
  [kUi_Core1]           = {"CORE1",         "COEUR1", "NÚCLEO 1", "NÚCLEO 1", "CORE 1", "KERN 1", "ЯДРО 1", "核心 1" },
  [kUi_Room]            = {"ROOM",          "SALLE", "SALA", "SALA", "STANZA", "RAUM", "КОМНАТА", "房间" },
  [kUi_Module]          = {"MODULE",        "MODULE", "MÓDULO", "MÓDULO", "MODULO", "MODUL", "МОДУЛЬ", "模块" },
  [kUi_House]           = {"HOUSE",         "MAISON", "CASA", "CASA", "CASA", "HAUS", "ДОМ", "房屋" },
  [kUi_Overworld]       = {"OVERWORLD",     "MONDE", "MUNDO", "MUNDO", "MONDO", "WELT", "МИР", "地上世界" },

  [kUi_RemapButtons]    = {"REMAP BUTTONS", "TOUCHES", "CONTROLES", "CONTROLES", "COMANDI", "TASTEN", "КНОПКИ", "按键映射" },
  [kUi_More]            = {"MORE",          "SUITE", "MÁS", "MAIS", "ALTRO", "MEHR", "ДАЛЕЕ", "更多" },
  [kUi_Top]             = {"TOP",           "D\xC3\x89""BUT", "INICIO", "INÍCIO", "INIZIO", "ANFANG", "ВВЕРХ", "顶部" },
  [kUi_PressKey]        = {"PRESS KEY",     "APPUYEZ", "PULSA", "PRESSIONE", "PREMI", "DRÜCKEN", "НАЖМИТЕ", "请按键" },

  [kUi_Channel]         = {"CHANNEL",       "CANAL", "CANAL", "CANAL", "CANALE", "KANAL", "КАНАЛ", "通道" },
  [kUi_Stable]          = {"STABLE",        "STABLE", "ESTABLE", "ESTÁVEL", "STABILE", "STABIL", "СТАБИЛЬНЫЙ", "稳定版" },
  [kUi_Prerelease]      = {"PRE-RELEASE",   "PR\xC3\x89VERSION", "PRUEBAS", "PRÉVIA", "ANTEPRIMA", "VORAB", "ТЕСТОВЫЙ", "预发布" },
  [kUi_NoRelease]       = {"NO RELEASE SELECTED",  "AUCUNE VERSION CHOISIE", "SIN VERSIÓN", "SEM VERSÃO", "NESSUNA VERSIONE", "KEINE VERSION", "ВЕРСИЯ НЕ ВЫБРАНА", "未选择版本" },
  [kUi_DownloadAndInstall] = {"DOWNLOAD AND INSTALL", "T\xC3\x89L\xC3\x89""CHARGER ET INSTALLER", "DESCARGAR E INSTALAR", "BAIXAR E INSTALAR", "SCARICA E INSTALLA", "LADEN UND INSTALLIEREN", "ЗАГРУЗИТЬ И УСТАНОВИТЬ", "下载并安装" },
  [kUi_CheckForUpdate]  = {"CHECK FOR UPDATE",     "CHERCHER UNE MISE \xC3\x80 JOUR", "BUSCAR ACTUALIZACIÓN", "BUSCAR ATUALIZAÇÃO", "CERCA AGGIORNAMENTI", "UPDATE SUCHEN", "ПРОВЕРИТЬ ОБНОВЛЕНИЯ", "检查更新" },
  [kUi_DownloadUpdate]  = {"DOWNLOAD UPDATE",      "T\xC3\x89L\xC3\x89""CHARGER", "DESCARGAR", "BAIXAR", "SCARICA", "HERUNTERLADEN", "ЗАГРУЗИТЬ", "下载更新" },
  [kUi_InstallUpdate]   = {"INSTALL UPDATE",       "INSTALLER", "INSTALAR", "INSTALAR", "INSTALLA", "INSTALLIEREN", "УСТАНОВИТЬ", "安装更新" },
  [kUi_PleaseWait]      = {"PLEASE WAIT",          "PATIENTEZ", "ESPERA", "AGUARDE", "ATTENDI", "BITTE WARTEN", "ПОДОЖДИТЕ", "请稍候" },
  [kUi_UpdateInstalled] = {"UPDATE INSTALLED",     "MISE \xC3\x80 JOUR INSTALL\xC3\x89""E", "ACTUALIZACIÓN INSTALADA", "ATUALIZAÇÃO INSTALADA", "AGGIORNAMENTO INSTALLATO", "UPDATE INSTALLIERT", "ОБНОВЛЕНИЕ УСТАНОВЛЕНО", "更新已安装" },
  [kUi_NotesOnTop]      = {"CHANGELOG ON TOP SCREEN",   "NOTES SUR L\x27\xC3\x89""CRAN DU HAUT", "CAMBIOS EN PANTALLA SUPERIOR", "NOTAS NA TELA SUPERIOR", "NOVITÀ SULLO SCHERMO ALTO", "NEUERUNGEN OBEN", "ИЗМЕНЕНИЯ НА ВЕРХНЕМ ЭКРАНЕ", "更新说明在上屏" },
  [kUi_TapForNotes]     = {"TAP THE RELEASE FOR CHANGELOG", "TOUCHEZ LA VERSION POUR LES NOTES", "TOCA LA VERSIÓN PARA VER CAMBIOS", "TOQUE A VERSÃO PARA VER NOTAS", "TOCCA LA VERSIONE PER LE NOVITÀ", "VERSION FÜR NEUERUNGEN ANTIPPEN", "КОСНИТЕСЬ ВЕРСИИ ДЛЯ ОПИСАНИЯ", "点按版本查看更新说明" },
  [kUi_Prev]            = {"PREV",          "PR\xC3\x89""C.", "ANT.", "ANT.", "PREC.", "ZURÜCK", "НАЗАД", "上一页" },
  [kUi_Next]            = {"NEXT",          "SUIV.", "SIG.", "PRÓX.", "SUCC.", "WEITER", "ДАЛЕЕ", "下一页" },
  [kUi_SystemUpdates]   = {"3DS UPDATES",   "MISES \xC3\x80 JOUR 3DS", "ACTUALIZACIONES 3DS", "ATUALIZAÇÕES 3DS", "AGGIORNAMENTI 3DS", "3DS UPDATES", "ОБНОВЛЕНИЯ 3DS", "3DS 更新" },
  [kUi_UpdateAvailable] = {"UPDATE AVAILABLE - OPEN SETTINGS",
                            "MISE \xC3\x80 JOUR DISPONIBLE - VOIR OPTIONS", "ACTUALIZACIÓN DISPONIBLE", "ATUALIZAÇÃO DISPONÍVEL", "AGGIORNAMENTO DISPONIBILE", "UPDATE VERFÜGBAR", "ЕСТЬ ОБНОВЛЕНИЕ", "有更新 - 打开设置" },

  [kUi_PickReleaseLine1] = {"SELECT A RELEASE ON THE TOUCH SCREEN",
                             "CHOISISSEZ UNE VERSION SUR L'\xC3\x89""CRAN TACTILE", "ELIGE UNA VERSIÓN ABAJO", "ESCOLHA UMA VERSÃO ABAIXO", "SCEGLI UNA VERSIONE IN BASSO", "UNTEN EINE VERSION WÄHLEN", "ВЫБЕРИТЕ ВЕРСИЮ ВНИЗУ", "在触摸屏上选择版本" },
  [kUi_PickReleaseLine2] = {"TO READ ITS CHANGELOG HERE.",
                             "POUR LIRE SES NOTES ICI.", "PARA LEER SUS CAMBIOS AQUÍ.", "PARA LER AS NOTAS AQUI.", "PER LEGGERE QUI LE NOVITÀ.", "UM DIE NEUERUNGEN ZU LESEN.", "ЧТОБЫ ПРОЧИТАТЬ ИЗМЕНЕНИЯ.", "以在此处查看更新说明。" },
};

// Dungeons in game order. Names are wrapped or use the small native face.
static const char *const kUiDungeonNames[14][kUiLanguageCount] = {
  { "SEWERS",         "\xC3\x89GOUTS", "ALCANTARILLAS", "ESGOTOS", "FOGNE", "KANALISATION", "КАНАЛИЗАЦИЯ"  , "下水道" },
  { "HYRULE CASTLE",  "CHATEAU D'HYRULE", "CASTILLO DE HYRULE", "CASTELO DE HYRULE", "CASTELLO DI HYRULE", "SCHLOSS HYRULE", "ЗАМОК ХАЙРУЛА"  , "海拉尔城" },
  { "EASTERN PALACE", "PALAIS DE L'EST", "PALACIO DEL ESTE", "PALÁCIO DO LESTE", "PALAZZO ORIENTALE", "OSTPALAST", "ВОСТОЧНЫЙ ДВОРЕЦ"  , "东方宫殿" },
  { "DESERT PALACE",  "PALAIS DU D\xC3\x89SERT", "PALACIO DEL DESIERTO", "PALÁCIO DO DESERTO", "PALAZZO DEL DESERTO", "WÜSTENPALAST", "ПУСТЫННЫЙ ДВОРЕЦ"  , "沙漠宫殿" },
  { "CASTLE TOWER",   "TOUR DU CHATEAU", "TORRE DEL CASTILLO", "TORRE DO CASTELO", "TORRE DEL CASTELLO", "SCHLOSSTURM", "БАШНЯ ЗАМКА"  , "城堡塔" },
  { "SWAMP PALACE",   "PALAIS DES MARAIS", "PALACIO DEL PANTANO", "PALÁCIO DO PÂNTANO", "PALAZZO DELLA PALUDE", "SUMPFPALAST", "БОЛОТНЫЙ ДВОРЕЦ"  , "沼泽宫殿" },
  { "DARK PALACE",    "PALAIS DES T\xC3\x89N\xC3\x88""BRES", "PALACIO OSCURO", "PALÁCIO DAS TREVAS", "PALAZZO OSCURO", "DUNKELPALAST", "ТЁМНЫЙ ДВОРЕЦ"  , "黑暗宫殿" },
  { "MISERY MIRE",    "MARAIS SANS FIN", "PALACIO DE MISERY MIRE", "PALÁCIO DO PÂNTANO SOMBRIO", "PALAZZO DELLA PALUDE OSCURA", "ELENDSSUMPF", "БОЛОТО СТРАДАНИЙ"  , "苦难沼泽" },
  { "SKULL WOODS",    "BOIS DU CRANE", "PALACIO DE LA CALAVERA", "PALÁCIO DA CAVEIRA", "PALAZZO DEL TESCHIO", "SCHÄDELWALD", "ЛЕС КОСТЕЙ"  , "骷髅森林" },
  { "ICE PALACE",     "PALAIS DE GLACE", "PALACIO DE HIELO", "PALÁCIO DE GELO", "PALAZZO DI GHIACCIO", "EISPALAST", "ЛЕДЯНОЙ ДВОРЕЦ"  , "冰之宫殿" },
  { "TOWER OF HERA",  "TOUR D'H\xC3\x89RA", "TORRE DE HERA", "TORRE DE HERA", "TORRE DI HERA", "HERATURM", "БАШНЯ ГЕРЫ"  , "赫拉之塔" },
  { "THIEVES TOWN",   "VILLAGE DES VOLEURS", "PUEBLO DE LADRONES", "VILA DOS LADRÕES", "VILLAGGIO DEI LADRI", "DIEBESVERSTECK", "ГОРОД ВОРОВ"  , "盗贼镇" },
  { "TURTLE ROCK",    "ROCHER DE LA TORTUE", "ROCA DE LA TORTUGA", "ROCHA DA TARTARUGA", "ROCCIA DELLA TARTARUGA", "SCHILDKRÖTENFELS", "ЧЕРЕПАШЬЯ СКАЛА"  , "龟岩" },
  { "GANONS TOWER",   "TOUR DE GANON", "TORRE DE GANON", "TORRE DE GANON", "TORRE DI GANON", "GANONS TURM", "БАШНЯ ГАНОНА"  , "加农之塔" },
};

static int g_ui_language = kUiLanguage_English;

void UI_SetLanguage(int language) {
  g_ui_language = (language > 0 && language < kUiLanguageCount) ? language
                                                                : kUiLanguage_English;
}

int UI_GetLanguage(void) {
  return g_ui_language;
}

const char *UI_Text(int id) {
  if (id < 0 || id >= kUiStringCount)
    return "";
  const char *text = kUiStrings[id][g_ui_language];
  if (!text)
    text = kUiStrings[id][kUiLanguage_English];
  return text ? text : "";
}

const char *UI_LanguageCode(int language) {
  static const char *const codes[] = {"en", "fr", "es", "pt", "it", "de", "ru", "zh"};
  return codes[language >= 0 && language < kUiLanguageCount ? language : 0];
}

const char *UI_DungeonName(int dungeon) {
  if (dungeon < 0 || dungeon >= 14)
    return UI_Text(kUi_Dungeon);
  return kUiDungeonNames[dungeon][g_ui_language];
}

int UI_LanguageFromCode(const char *code) {
  if (code) for (int i = 0; i < kUiLanguageCount; i++)
    if (strcmp(code, UI_LanguageCode(i)) == 0) return i;
  return kUiLanguage_English;
}

const char *UI_UpdateError(const char *message) {
  static const struct { const char *message; int id; } errors[] = {
    {"SD CARD WRITE FAILED", kUi_ErrorSdWrite},
    {"CANNOT WRITE TO SD CARD", kUi_ErrorSdWrite},
    {"NO WI-FI CONNECTION", kUi_ErrorWifi},
    {"TLS SERVICE FAILED", kUi_ErrorTls},
    {"INVALID RELEASE DATA", kUi_ErrorRelease},
    {"NOT ENOUGH SD SPACE", kUi_ErrorSpace},
    {"3DSX LAUNCH PATH UNKNOWN", kUi_ErrorLaunchPath},
    {"DOWNLOAD CHECK FAILED", kUi_ErrorDownload},
    {"INSTALLATION FAILED", kUi_ErrorInstall},
    {"UPDATE CANCELLED", kUi_ErrorCancelled},
    {"CONNECTION FAILED - RETRY", kUi_ErrorConnection},
    {"CANNOT START UPDATE", kUi_ErrorStart},
    {"CANNOT SAVE UPDATE CHANNEL", kUi_ErrorChannel},
  };
  if (!message) return "";
  for (unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++)
    if (strcmp(message,errors[i].message)==0) return UI_Text(errors[i].id);
  return message;
}
