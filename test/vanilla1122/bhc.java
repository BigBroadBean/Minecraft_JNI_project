// 测试用假客户端 (原版 1.12.2 混淆运行时) — RayTraceResult (bhc)
public class bhc {
    public bhc$a a;   // typeOfHit
    public vg d;      // entityHit

    public bhc() {
        this.a = bhc$a.a; // MISS
    }

    public bhc(vg entityHit) {
        this.d = entityHit;
        this.a = bhc$a.c; // ENTITY
    }
}
