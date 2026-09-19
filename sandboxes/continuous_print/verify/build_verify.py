from pathlib import Path
import os,subprocess,sys
paths=[Path('build/src/OrcaSlicer.vcxproj'),Path('build/tests/libslic3r/libslic3r_tests.vcxproj')]
original={p:p.read_bytes() for p in paths}
try:
 for p,b in original.items():p.write_bytes(b.replace(b'<GenerateDebugInformation>true</GenerateDebugInformation>',b'<GenerateDebugInformation>false</GenerateDebugInformation>'))
 with open(sys.argv[1],'w') as log:
  r=subprocess.run(['/mnt/c/Program Files/CMake/bin/cmake.exe','--build','G:\\SinglePath\\OrcaSlicer\\build','--config','Release','--target','OrcaSlicer','libslic3r_tests','--','/maxcpucount:2'],env=dict(os.environ,WSLENV='_CL_',_CL_='/Z7 /Y-'),stdout=log,stderr=subprocess.STDOUT)
 print('build exit',r.returncode)
finally:
 for p,b in original.items():p.write_bytes(b)
 print('restored generated project settings')
sys.exit(r.returncode)
