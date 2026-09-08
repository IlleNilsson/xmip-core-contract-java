package xmip.contract;

/**
 * The Java content contract - a technology of xmip-core-contract, in Java.
 *
 * <p>ADR-0042 decision 3: a contract may be authored in any declared language
 * over the C ABI. A JVM cannot export the C entrypoint itself, so the shim in
 * {@code shim/} does: it hosts this JVM in-process through JNI and forwards the
 * contract table to this class (owner, 2026-09-07). Nothing here touches Xmip's
 * Rust or its C header; the shim is the only thing that does.
 *
 * <p>What it claims: well-formedness is bytes (ADR-0042 decision 1). A Java
 * contract with a real standard replaces {@link #validate} and nothing else.
 */
public final class Contract {
    private Contract() {}

    /**
     * Judge a whole stream against the bound descriptor.
     *
     * @param descriptor what the Location bound, empty when nothing
     * @param bytes the stream, read to its end
     * @return empty when the stream holds, else the message the diagnostic carries
     */
    public static String validate(String descriptor, byte[] bytes) {
        return "";
    }

    /**
     * What the bound contract already determines about a key, or null.
     */
    public static String implies(String descriptor, String key) {
        if ("descriptor".equals(key) && descriptor != null && !descriptor.isEmpty()) {
            return descriptor;
        }
        return null;
    }
}
