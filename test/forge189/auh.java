// 测试用假客户端 (Forge 1.8.9 SRG 运行时)
public class auh {
    public enum a { MISS, BLOCK, ENTITY }

    public a field_72313_a;
    public pk field_72308_g;

    public auh() {
        this.field_72313_a = a.MISS;
    }

    public auh(pk entityHit) {
        this.field_72308_g = entityHit;
        this.field_72313_a = a.ENTITY;
    }
}
