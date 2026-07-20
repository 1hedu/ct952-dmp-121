import struct,sys
s=open(sys.argv[1],'rb').read()
base=[i for i in range(0,len(s)-3) if struct.unpack('>I',s[i:i+4])[0]==0x591b4][0]-0x20e18
def off(a): return base+(a-0x40000000)
def b(a): return s[off(a)]
def w(a): return struct.unpack('>I',s[off(a):off(a)+4])[0]
tag=sys.argv[2] if len(sys.argv)>2 else '?'
print("=== %s ==="%tag)
print(" __bPOWERONMENUInitial(0x40023a10) = %d"%b(0x40023a10))
print(" _bOSDSSScreenSaverMode(0x400239c4)= %d   <== ACCEPTANCE (1=screensaver armed)"%b(0x400239c4))
print(" __dwTimeNow(0x40031abc)           = 0x%08x"%w(0x40031abc))
print(" CheckNOData(0x400239c0)           = 0x%08x"%w(0x400239c0))
print(" powerdown(0x4002fb58)             = %d"%b(0x4002fb58))
print(" CLOCKShowClock(0x40020ff4)        = %d"%b(0x40020ff4))
print(" AlarmState(0x4002f7c6)            = %d"%b(0x4002f7c6))
au=w(0x40020ec8)
print(" activeUI(0x40020ec8)              = 0x%08x"%au)
