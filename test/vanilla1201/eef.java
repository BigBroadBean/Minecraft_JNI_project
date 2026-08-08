// 测试用假客户端 (原版/Fabric 1.20.1 混淆运行时)
public class eef extends eeg {
    private final bfj b;

    public eef(bfj entity) {
        this.b = entity;
    }

    public bfj a() {
        return this.b;
    }

    public a c() {
        return a.c; // ENTITY
    }
}
