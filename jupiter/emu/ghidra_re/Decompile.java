// Ghidra headless post-script: decompile a list of function addresses to C.
// Addresses passed via the CT952_DECOMP env var (comma-separated hex).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decompile extends GhidraScript {
    public void run() throws Exception {
        String list = System.getenv("CT952_DECOMP");
        if (list == null || list.isEmpty()) { println("no CT952_DECOMP"); return; }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String s : list.split(",")) {
            s = s.trim();
            if (s.isEmpty()) continue;
            long a = Long.parseLong(s.replace("0x",""), 16);
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a);
            Function f = getFunctionContaining(addr);
            if (f == null) {
                f = createFunction(addr, "func_" + s);
            }
            println("==== FUNC containing " + s + " ====");
            if (f == null) { println("(could not create function)"); continue; }
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            if (r != null && r.decompileCompleted()) {
                println(r.getDecompiledFunction().getC());
            } else {
                println("(decompile failed: " + (r==null?"null":r.getErrorMessage()) + ")");
            }
        }
    }
}
