"""Render the checked-in audit Markdown as a standalone readable PDF."""
from pathlib import Path
import re, html
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, PageBreak, Table, TableStyle, Preformatted, KeepTogether
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
ROOT=Path(__file__).resolve().parent
FONT=Path('/usr/share/fonts/truetype/dejavu')
for name,file in [('Body','DejaVuSans.ttf'),('Bold','DejaVuSans-Bold.ttf'),('Mono','DejaVuSansMono.ttf')]:
 pdfmetrics.registerFont(TTFont(name,str(FONT/file)))
pdfmetrics.registerFontFamily('Body',normal='Body',bold='Bold',italic='Body',boldItalic='Bold')
ink=colors.HexColor('#172333');blue=colors.HexColor('#2862cc');muted=colors.HexColor('#627082')
styles=getSampleStyleSheet()
styles.add(ParagraphStyle(name='AuditBody',fontName='Body',fontSize=9,leading=13.4,textColor=ink,spaceAfter=7))
styles.add(ParagraphStyle(name='AuditH1',fontName='Bold',fontSize=17,leading=22,textColor=ink,spaceBefore=18,spaceAfter=11,keepWithNext=True))
styles.add(ParagraphStyle(name='AuditH2',fontName='Bold',fontSize=11.5,leading=16,textColor=blue,spaceBefore=13,spaceAfter=7,keepWithNext=True))
styles.add(ParagraphStyle(name='AuditBullet',parent=styles['AuditBody'],leftIndent=13,firstLineIndent=-10))
styles.add(ParagraphStyle(name='AuditCell',parent=styles['AuditBody'],fontSize=8,leading=11,spaceAfter=0))
styles.add(ParagraphStyle(name='AuditCode',fontName='Mono',fontSize=7,leading=10,textColor=ink,spaceBefore=6,spaceAfter=9))
styles.add(ParagraphStyle(name='CoverTitle',fontName='Bold',fontSize=32,leading=39,textColor=ink,spaceAfter=22))
styles.add(ParagraphStyle(name='CoverSub',fontName='Body',fontSize=14,leading=22,textColor=muted,spaceAfter=20))
def inline(s):
 s=s.replace('—',' - ').replace('–','-').replace('‑','-').replace('→',' -> ')
 s=html.escape(s)
 s=re.sub(r'\[([^\]]+)\]\(([^)]+)\)',r'<link href="\2" color="#2862cc">\1</link>',s)
 s=re.sub(r'`([^`]+)`',r'<font name="Mono" size="8">\1</font>',s)
 s=re.sub(r'\*\*([^*]+)\*\*',r'<b>\1</b>',s)
 s=re.sub(r'(?<!\*)\*([^*]+)\*(?!\*)',r'<i>\1</i>',s)
 return s
W,H=595.28,841.89
flow=[]
bar=Table([['','','','']],colWidths=[120]*4,rowHeights=[6]);bar.setStyle(TableStyle([('BACKGROUND',(i,0),(i,0),colors.HexColor(c)) for i,c in enumerate(['#22b35c','#3b7bff','#ea4335','#f6c21c'])]));flow+=[bar,Spacer(1,55)]
flow+=[Paragraph('PHOTON64',styles['AuditH2']),Paragraph('Remaining-defect audit<br/>and implementation plan',styles['CoverTitle']),Paragraph('Code-grounded review of the emulator, browser app and validation pipeline.',styles['CoverSub']),Spacer(1,20)]
for s in ['10 October 2026 | Source 96a7f073dffaa8cf002b3e5bb83f20ce37da66d3','18 findings | 10 reproduced defect classes | 9 implementation phases','Primary priorities: save integrity, GPU ownership and trustworthy browser evidence.']:
 flow.append(Paragraph(inline(s),styles['AuditBody']))
flow += [Spacer(1,40),Paragraph('Review status',styles['AuditH2']),Paragraph('The latest exact-source CI run passed seven jobs and failed native Safari. This report includes targeted reproductions, confidence levels and acceptance criteria. Production code was not changed by this audit.',styles['AuditBody']),Spacer(1,24),Paragraph('Contents',styles['AuditH2'])]
for s in ['1. Executive assessment','2. Evidence and coverage','3. Findings F01-F18','4. Step-by-step implementation plan','5. Acceptance and delivery discipline','6. Reproduction guide','7. Source references']:
 flow.append(Paragraph(s,styles['AuditBody']))
flow.append(PageBreak())
lines=(ROOT/'REPORT.md').read_text().splitlines();i=1
while i<len(lines):
 line=lines[i].strip()
 if not line:i+=1;continue
 if line.startswith('```'):
  code=[];i+=1
  while i<len(lines) and not lines[i].startswith('```'):
   code.append(lines[i]);i+=1
  import textwrap
  code='\n'.join('\n'.join(textwrap.wrap(x,104,replace_whitespace=False,drop_whitespace=False)) or '' for x in code)
  flow.append(Preformatted(code,styles['AuditCode']));i+=1;continue
 if line.startswith('|'):
  rows=[]
  while i<len(lines) and lines[i].startswith('|'):
   cells=[x.strip() for x in lines[i].strip().strip('|').split('|')]
   if not all(re.fullmatch(r'[-: ]+',c) for c in cells): rows.append([Paragraph(inline(c),styles['AuditCell']) for c in cells])
   i+=1
  t=Table(rows,colWidths=[143,47,290],repeatRows=1,hAlign='LEFT')
  t.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,0),colors.HexColor('#e9eff7')),('VALIGN',(0,0),(-1,-1),'TOP'),('BOX',(0,0),(-1,-1),.4,colors.HexColor('#d2dbe6')),('LINEBELOW',(0,0),(-1,0),.5,colors.HexColor('#b6c4d5')),('ROWBACKGROUNDS',(0,1),(-1,-1),[colors.white,colors.HexColor('#f6f8fb')]),('LEFTPADDING',(0,0),(-1,-1),7),('RIGHTPADDING',(0,0),(-1,-1),7),('TOPPADDING',(0,0),(-1,-1),7),('BOTTOMPADDING',(0,0),(-1,-1),7)]));flow.append(t);flow.append(Spacer(1,10));continue
 if line.startswith('## '):
  title=line[3:]
  if title.startswith(('3. ','4. ','6. ')):flow.append(PageBreak())
  flow.append(Paragraph(inline(title),styles['AuditH1']));i+=1;continue
 if line.startswith('### '):flow.append(Paragraph(inline(line[4:]),styles['AuditH2']));i+=1;continue
 if line.startswith('- '):flow.append(Paragraph('• '+inline(line[2:]),styles['AuditBullet']));i+=1;continue
 if re.match(r'^\d+\. ',line):flow.append(Paragraph(inline(line),styles['AuditBullet']));i+=1;continue
 para=[line];i+=1
 while i<len(lines) and lines[i].strip() and not re.match(r'^(#|\||```|- |\d+\. )',lines[i]):para.append(lines[i].strip());i+=1
 flow.append(Paragraph(inline(' '.join(para)),styles['AuditBody']))
def footer(c,d):
 c.saveState();c.setStrokeColor(colors.HexColor('#d8e0e9'));c.line(56,45,W-56,45)
 c.setFont('Body',7);c.setFillColor(muted);c.drawString(56,31,'Photon64 audit | 10 October 2026 | 96a7f07');c.drawRightString(W-56,31,str(d.page));c.restoreState()
out=ROOT/'Photon64_Audit_2026-10-10.pdf'
doc=SimpleDocTemplate(str(out),pagesize=(W,H),leftMargin=56,rightMargin=56,topMargin=48,bottomMargin=59,title='Photon64 audit and implementation plan - 10 October 2026',author='Photon64 project review')
doc.build(flow,onFirstPage=footer,onLaterPages=footer)
print(out)
