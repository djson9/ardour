#!/usr/bin/env python3
"""Install or restore the Charcoal visual/layout preset in a stopped macOS dev app."""
import argparse, os, plistlib, shutil, subprocess, xml.etree.ElementTree as ET
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--app',type=Path,default=Path.home()/'Applications/Ardour9.app')
p.add_argument('--preferences',type=Path,default=Path.home()/'Library/Preferences/Ardour9')
p.add_argument('--layout',action='store_true',help='Browser left, track headers right, properties below')
p.add_argument('--restore',action='store_true')
a=p.parse_args()
app=a.app.resolve();prefs=a.preferences.resolve()
if subprocess.run(['pgrep','-f','^'+str(app/'Contents/MacOS/Ardour9')],stdout=subprocess.DEVNULL).returncode==0:
    raise SystemExit('Save and quit Ardour before applying the profile')
backup=prefs/'charcoal-profile-backup'
ui=prefs/'ui_config';plist=app/'Contents/Info.plist'
if a.restore:
    if not (backup/'ui_config').exists() or not (backup/'Info.plist').exists():raise SystemExit('No profile backup exists')
    shutil.copy2(backup/'ui_config',ui);shutil.copy2(backup/'Info.plist',plist)
else:
    backup.mkdir(parents=True,exist_ok=True)
    for src,dst in [(ui,backup/'ui_config'),(plist,backup/'Info.plist')]:
        if not dst.exists():shutil.copy2(src,dst)
    source=Path(__file__).resolve().parents[2]/'gtk2_ardour/themes/charcoal-ardour.colors'
    # Themes are discovered in the bundle's data path. Keep the original themes.
    dest=app/'Contents/Resources/themes/charcoal-ardour.colors'
    ET.parse(source);shutil.copy2(source,dest)
    tree=ET.parse(ui);section=tree.getroot().find('UI')
    if section is None:section=ET.SubElement(tree.getroot(),'UI')
    options={'color-file':'charcoal','flat-buttons':'1','boxy-buttons':'1','timeline-item-gradient-depth':'0','color-regions-using-track-color':'1','use-route-color-widely':'1','render-plus-hints':'0'}
    for name,value in options.items():
        node=next((x for x in section if x.get('name')==name),None)
        if node is None:node=ET.SubElement(section,'Option',{'name':name})
        node.set('value',value)
    tree.write(ui,encoding='UTF-8',xml_declaration=True)
    with plist.open('rb') as f:data=plistlib.load(f)
    env=data.setdefault('LSEnvironment',{})
    if a.layout:env['ARDOUR_LIVE_LAYOUT']='1'
    else:env.pop('ARDOUR_LIVE_LAYOUT',None)
    with plist.open('wb') as f:plistlib.dump(data,f)
subprocess.run(['codesign','--force','--sign','-',str(app)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
print('Restored previous appearance' if a.restore else 'Installed Charcoal with '+('Live layout' if a.layout else 'standard layout'))
print('Backup:',backup)
