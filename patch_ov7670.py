Import("env")
import os

lib_dir=os.path.join(env.subst("$PROJECT_DIR"),"lib","TARS-OV7670")
cpp_file=os.path.join(lib_dir,"I2SCamera.cpp")
h_file=os.path.join(lib_dir,"I2SCamera.h")

print("")
print("========== TARS OV7670 STREAM PATCH ==========")
print("LIB :",lib_dir)

if not os.path.isfile(cpp_file):
    print("TARS ERROR: I2SCamera.cpp not found")
    env.Exit(1)

if not os.path.isfile(h_file):
    print("TARS ERROR: I2SCamera.h not found")
    env.Exit(1)

print("TARS: I2SCamera.cpp FOUND")
print("TARS: size =",os.path.getsize(cpp_file),"bytes")

with open(h_file,"r",encoding="utf-8") as f:
    hdata=f.read()

old_gpio_decl="void gpio_matrix_in(int gpio, int signal_index, bool inverted);"
if old_gpio_decl in hdata:
    hdata=hdata.replace(old_gpio_decl,"")
    print("TARS: gpio_matrix_in conflict patched")
else:
    print("TARS: gpio_matrix_in patch not needed")

if "~I2SCamera()" not in hdata:
    marker="class I2SCamera\n{\npublic:\n"
    if marker in hdata:
        hdata=hdata.replace(
            marker,
            marker+"  ~I2SCamera(){deinit();}\n",
            1
        )
        print("TARS: I2SCamera destructor added")
    else:
        print("TARS: destructor marker not found - skipped")
else:
    print("TARS: I2SCamera destructor already present")

with open(h_file,"w",encoding="utf-8") as f:
    f.write(hdata)

print("TARS: I2SCamera.cpp NOT MODIFIED")
print("TARS: STREAMING SOURCE PRESERVED")
print("TARS: TARGET QVGA 320x240 RGB565")
print("TARS: BLOCK 16 lines x 4")
print("========== TARS OV7670 STREAM PATCH DONE ==========")
