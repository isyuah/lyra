import self.parseLine

fn main(): Unit {
    val good = parseLine("{\"station\":\"north\",\"delta\":-3}")
    #assert(match good {
        Ok(reading) => reading.station == "north" && reading.delta == -3,
        Err(_) => false,
    }, "valid reading")

    val bad = parseLine("{\"station\":\"north\"}")
    #assert(match bad { Err(_) => true, Ok(_) => false }, "missing measurement")
}
