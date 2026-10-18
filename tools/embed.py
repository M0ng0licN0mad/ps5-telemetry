import sys
data = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as f:
    f.write("static const unsigned char DASHBOARD_HTML[] = {\n")
    for i in range(0, len(data), 16):
        f.write(",".join(str(b) for b in data[i:i+16]) + ",\n")
    f.write("0};\nstatic const unsigned DASHBOARD_LEN = %d;\n" % len(data))
