import xmip.contract.Contract;

/** Plain-Java checks, no framework: the JDK is the only prerequisite. */
public final class ContractTest {
    private static int failures = 0;

    private static void check(boolean condition, String what) {
        if (condition) {
            System.out.println("ok  " + what);
        } else {
            System.err.println("FAILED: " + what);
            failures++;
        }
    }

    public static void main(String[] args) {
        check(Contract.validate("any", new byte[] {0, 1, (byte) 0xff}).isEmpty(),
              "the identity contract holds everything");
        check("any".equals(Contract.implies("any", "descriptor")),
              "implies answers the bound descriptor");
        check(Contract.implies("any", "nothing") == null,
              "and null for what it does not determine");
        System.out.println(failures == 0 ? "OK" : "FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
