"""Run one explicit command in the GUI bootstrap domain and collect its output."""
import pathlib,plistlib,subprocess,sys,os,time
root=pathlib.Path(__file__).resolve().parents[2];out=root/'artifacts/anticipation-gui';label='dev.ardour.anticipation-gui-command'
job=dict(Label=label,ProgramArguments=sys.argv[1:],RunAtLoad=True,StandardOutPath=str(out/'command.log'),StandardErrorPath=str(out/'command.log'))
p=out/'command.plist';p.write_bytes(plistlib.dumps(job));(out/'command.log').write_text('')
subprocess.run(['launchctl','bootout',f'gui/{os.getuid()}/{label}'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
subprocess.run(['launchctl','bootstrap',f'gui/{os.getuid()}',str(p)],check=True)
time.sleep(1)
print((out/'command.log').read_text())
subprocess.run(['launchctl','bootout',f'gui/{os.getuid()}/{label}'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
