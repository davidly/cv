@echo off
del cv.exe
del cv.pdb
cl /nologo cv.cxx /I.\ /O2it /EHac /Zi /Gy /D_AMD64_ /link ntdll.lib /OPT:REF


