// 测试用假客户端 (原版 1.8.9 混淆运行时)
public class auh {
    public auh$a a;
    public pk d;

    public auh() {
        this.a = auh$a.a; // MISS
    }

    public auh(pk entityHit) {
        this.d = entityHit;
        this.a = auh$a.c; // ENTITY
    }
}
