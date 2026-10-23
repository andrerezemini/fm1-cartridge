"""Build the illustrated French quick guide from the actual host-rendered UI."""
from pathlib import Path
from reportlab.pdfgen import canvas
from reportlab.lib.colors import HexColor
from reportlab.lib.styles import ParagraphStyle
from reportlab.platypus import Paragraph, Table, TableStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'output/pdf/Studio-0.3-guide-rapide-FR.pdf'
OUT.parent.mkdir(parents=True, exist_ok=True)
pdfmetrics.registerFont(TTFont('Guide', 'C:/Windows/Fonts/arial.ttf'))
pdfmetrics.registerFont(TTFont('GuideBold', 'C:/Windows/Fonts/arialbd.ttf'))
pdfmetrics.registerFontFamily('Guide', normal='Guide', bold='GuideBold')
C = canvas.Canvas(str(OUT), pagesize=(595.28, 841.89))
C.setTitle('Studio 0.3 DEV - Guide rapide du FM-1')
C.setAuthor('Studio - dérivé de Felucca')
INK = HexColor('#142636'); ACC = HexColor('#007b9e'); DIM = HexColor('#4b6171')
STYLE = ParagraphStyle('body', fontName='Guide', fontSize=10, leading=14, textColor=INK)
def p(text, y, x=46, w=503, size=10):
    style = ParagraphStyle('p', parent=STYLE, fontSize=size, leading=size*1.4)
    para = Paragraph(text, style)
    _, h = para.wrap(w, 1000)
    assert y-h > 43, (text[:40], y, h)
    para.drawOn(C,x,y-h)
    return y-h-10
def title(n, heading, subtitle):
    C.setFillColor(ACC);C.rect(0,825,595.28,17,fill=1,stroke=0)
    C.setFillColor(DIM);C.setFont('GuideBold',9);C.drawString(46,791,'FM-1  /  STUDIO 0.3 DEV')
    C.setFillColor(INK);C.setFont('GuideBold',24);C.drawString(46,757,heading)
    p(subtitle,739,size=10)
    C.setStrokeColor(HexColor('#d5e5eb'));C.line(46,38,549,38)
    C.setFont('Guide',8);C.setFillColor(DIM)
    C.drawString(46,25,'Guide rapide • Écrans rendus par le firmware • Version expérimentale')
    C.drawRightString(549,25,f'{n} / 4')
def heading(s,y):
    C.setFont('GuideBold',13);C.setFillColor(ACC);C.drawString(46,y,s);return y-12
def shot(name,x,y,size):
    C.drawImage(str(ROOT/'build/host'/f'{name}.png'),x,y,width=size,height=size)
def table(rows,y,widths):
    cells=[[Paragraph(str(c),ParagraphStyle('cell',parent=STYLE,fontSize=9,leading=12)) for c in row] for row in rows]
    t=Table(cells,colWidths=widths,hAlign='LEFT')
    t.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,0),HexColor('#e7f3f7')),('VALIGN',(0,0),(-1,-1),'TOP'),('TOPPADDING',(0,0),(-1,-1),7),('BOTTOMPADDING',(0,0),(-1,-1),7),('LINEBELOW',(0,0),(-1,-1),.4,HexColor('#d5e5eb'))]))
    _,h=t.wrap(503,800);assert y-h>45;t.drawOn(C,46,y-h);return y-h-15

title(1,'Votre premier morceau','Trois instruments + une batterie. HOME revient à la vue des quatre pistes.')
shot('studio-tracks',346,489,203)
y=699
y=p('<b>1. Poser la batterie</b><br/>Choisir la piste 4 avec ALGORITHM. Appuyer sur SEQ, puis encore SEQ pour ouvrir RYTHME.',y,w=279)
y=p('<b>2. Choisir une base</b><br/>KNOB 1 : style. KNOB 2 : A ou B. À l’arrêt, REC applique le rythme. S’il existe déjà un motif, confirmer avec un second REC sous 3 secondes.',y,w=279)
y=p('<b>3. Ajouter les instruments</b><br/>HOME, choisir une piste 1 à 3, puis son son avec PRESETS. Certains presets fournissent déjà un motif.',y,w=279)
y= min(y,470)
y=p('<b>4. Enregistrer vos notes</b><br/>Depuis HOME, REC arme la piste et démarre la lecture si nécessaire. Jouer le clavier ; REC désarme, PLAY arrête. Les passages s’ajoutent au motif. Pour effacer : à l’arrêt, maintenir REC sur HOME, puis OCT+ confirme ; OCT- annule.',y)
y=p('<b>5. Équilibrer</b><br/>Sur HOME : KNOB 1 choisit la piste, 2 son volume, 3 sa longueur, 4 son panoramique. SELECT règle le tempo. PLAY commande les quatre pistes ensemble.',y)
y=p('<b>6. Garder votre idée</b><br/>HOME puis SAVE ouvre CHANSON. Choisir A avec KNOB 2 ; REC mémorise les quatre pistes. Une section occupée demande un second REC. Préparer ensuite B, C et D pour construire la chanson.',y)
y=heading('Lire l’écran',y-5)
y=p('La barre blanche à gauche indique la piste choisie. Les carrés montrent son motif ; le petit trait blanc suit la lecture. La barre à droite indique le <b>volume réglé</b>, pas le niveau audio mesuré. REC signale une piste armée.',y)
C.showPage()

title(2,'La batterie, pas à pas','Depuis la piste 4 : SEQ ou EDIT ouvre la batterie. SEQ change d’onglet.')
shot('studio-grid',46,475,222)
shot('studio-kit',327,475,222)
y=453
y=table([['Onglet','KNOB 1','KNOB 2','KNOB 3','KNOB 4'],['GRILLE','Son','Pas','+ ajoute<br/>- retire','Vélocité'],['RYTHME','Style','A / B','Swing','Longueur'],['KIT','Couleur','Volume','Réverb.','Panoramique'] ],y,[90,103,90,110,110])
y=p('<b>GRILLE</b> : quatre lignes visibles parmi 12 sons. KNOB 1 fait défiler kick, snare, clap, hi-hats, toms, crash, ride, shaker, conga et rim. OCT-/OCT+ déplace le curseur de 16 pas. Un cadre blanc marque la cellule choisie.',y)
y=p('On peut ajouter ou retirer des frappes pendant une <b>boucle</b>. Pendant une chanson, arrêter avant d’éditer les frappes. Un pas accepte <b>4 sons maximum</b> ; sa vélocité est commune à ces sons.',y)
y=p('<b>Jouer en direct</b> : le clavier reste actif. Les neuf premières touches, de gauche à droite, jouent kick, kick grave, snare, snare alternative, rim, clap, hi-hat fermé, pédale et hi-hat ouvert. Les touches suivantes jouent toms, cymbales et percussions.',y)
y=p('REC dans GRILLE ou KIT arme/désarme l’enregistrement. PRESETS change de kit. SAVE ouvre CHANSON : y utiliser <b>REC pour mémoriser la section</b>. HOME revient aux pistes.',y)
C.showPage()

title(3,'Explorer plusieurs styles','Les rythmes sont des points de départ éditables, sur deux mesures en 4/4.')
y=700
y=table([['Style','Tempo conseillé','Couleur appliquée'],['House','124 BPM','Original'],['Techno','132 BPM','Tight'],['Hip hop','90 BPM','Dust'],['Trap','140 BPM','Deep'],['Rock','110 BPM','Original'],['Funk','105 BPM','Tight'],['Reggaeton','96 BPM','Bright'],['Drum & bass','172 BPM','Bright']],y,[196,151,156])
y=p('<b>A</b> garde la base. <b>B</b> ajoute un fill de toms en fin de deuxième mesure et un crash au début de cette mesure. Dans RYTHME, REC applique votre choix à l’arrêt. Un motif existant demande confirmation.',y)
y=p('Appliquer un style remplace seulement le motif de batterie et choisit son kit/swing : 32 pas, division 1/16. Cela ne modifie pas les trois instruments. Le tempo reste un <b>conseil</b> : le régler avec SELECT. Modifier la longueur ou le swing après l’application si souhaité.',y)
y=heading('Cinq couleurs de kit',y-4)
y=p('<b>ORIGINAL</b> : son existant, équilibré.<br/><b>DEEP</b> : accordage plus grave et aigus adoucis.<br/><b>TIGHT</b> : enveloppe raccourcie, frappes plus sèches.<br/><b>BRIGHT</b> : accordage relevé, caractère plus vif.<br/><b>DUST</b> : accordage légèrement abaissé, filtre et grain numérique.',y)
y=p('Ces couleurs traitent <b>la même base de samples</b> ; ce ne sont pas cinq banques enregistrées séparément. Le changement agit sur les nouvelles frappes. Le kit se sauvegarde avec la section.',y)
C.showPage()

title(4,'Assembler une chanson','Une chaîne jusqu’à 16 entrées, avec quatre sections A, B, C et D réutilisables.')
shot('song-screen',346,488,203)
y=700
y=p('<b>1. Mémoriser les sections</b><br/>À l’arrêt, HOME puis SAVE. KNOB 2 choisit A-D. REC mémorise les sons et les motifs des quatre pistes. Deux REC sous 3 secondes remplacent une section occupée.',y,w=278)
y=p('<b>2. Construire l’ordre</b><br/>KNOB 1 : entrée de la chaîne.<br/>KNOB 2 : section A-D.<br/>KNOB 3 : durée, de 1 à 64 mesures.<br/>KNOB 4 : nombre d’entrées, de 1 à 16.',y,w=278)
y=min(y,465)
y=p('<b>3. Sauvegarder et écouter</b><br/>SAVE mémorise l’ordre ; REC mémorise le contenu d’une section. OCT- passe de BOUCLE à SONG ; PLAY lance la chanson. Toutes les sections utilisées doivent exister. Arrêt automatique à la fin.',y)
y=p('<b>Exemple :</b> A pendant 4 mesures → B pendant 8 → A pendant 4 → C pendant 8. OCT+ recharge la section sélectionnée pour la modifier. Pour composer à nouveau, arrêter et revenir en BOUCLE.',y)
y=heading('Pour aller plus loin',y-2)
y=p('Sur une piste mélodique, SEQ ouvre STEP/PATTERN. STEP : KNOB 1 = pas, 2 = transposition, 3 = note/liaison/silence, 4 = accent/slide ; EDIT efface le pas. ENV, LFO, FX et EDIT donnent accès au son détaillé. HOME maintenu ouvre le menu. Les écrans avancés conservent des libellés Felucca.',y)
y=p('<b>Limites pratiques :</b> 8 voix de synthèse partagées + 6 voix de batterie. Quatre emplacements de section et un ordre de chanson sauvegardé. Tempo et effets globaux communs ; les motifs repartent ensemble à chaque section.',y)
y=heading('Version et secours',y-2)
y=p('Studio 0.3 conserve le secours USB expérimental : maintenir OCT- seul pendant l’allumage. Ce secours dépend d’un firmware suffisamment intact et ne garantit pas la récupération. Les nouveautés de 0.3 sont testées sur ordinateur ; leur essai sur ton FM-1 reste à faire. Studio 0.2 est conservé dans build/releases/studio-0.2-dev/.',y,size=9)
C.showPage();C.save();print(OUT)
