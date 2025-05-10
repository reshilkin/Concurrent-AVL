package contention.benchmark.workload.args.generators.impls;

import contention.benchmark.workload.args.generators.abstractions.ArgsGenerator;
import contention.benchmark.workload.data.map.abstractions.DataMap;
import contention.benchmark.workload.distributions.abstractions.Distribution;

public class ReadWriteArgsGenerator implements ArgsGenerator {
    private final DataMap data;
    private final Distribution readDistribution;
    private final Distribution writeDistribution;

    public ReadWriteArgsGenerator(DataMap data, Distribution readDistribution, Distribution writeDistribution) {
        this.data = data;
        this.readDistribution = readDistribution;
        this.writeDistribution = writeDistribution;
    }

    private int nextWrite() {
        int index = writeDistribution.next();
        return data.get(index);
    }

    @Override
    public int nextGet() {
        int index = readDistribution.next();
        return data.get(index);
    }

    @Override
    public int nextInsert() {
        return nextWrite();
    }

    @Override
    public int nextRemove() {
        return nextWrite();
    }

}
