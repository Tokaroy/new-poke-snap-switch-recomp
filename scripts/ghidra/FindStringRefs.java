// Print references to given module-relative addresses (hex, comma separated): FindStringRefs.java <out.txt> <hex,hex,...>
// Output: target, from address (module-relative), containing function entry, ref type.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class FindStringRefs extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        try (PrintWriter w = new PrintWriter(new FileWriter(a[0]))) {
            for (String s : a[1].split(",")) {
                long off = Long.parseLong(s.trim(), 16);
                Address t = toAddr(0x7100000000L + off);
                int n = 0;
                for (Reference r : currentProgram.getReferenceManager().getReferencesTo(t)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    w.println(Long.toHexString(off) + "\t" + Long.toHexString(r.getFromAddress().getOffset() - 0x7100000000L) + "\t" + (f == null ? "-" : Long.toHexString(f.getEntryPoint().getOffset() - 0x7100000000L)) + "\t" + r.getReferenceType());
                    n++;
                }
                w.println(Long.toHexString(off) + "\tcount\t" + n);
            }
        }
    }
}
