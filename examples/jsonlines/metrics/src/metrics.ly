#derive(Json)
pub struct Reading(pub val station: String, pub val delta: Int)

#derive(Json)
struct Summary(pub val rows: Int, pub val total: Int, pub val rejected: Int)

pub fn parseLine(line: String): Result<Reading,String> { decodeReading(line) }

pub fn summary(rows: Int, total: Int, rejected: Int): String {
    encodeSummary(Summary(rows, total, rejected))
}
