class User(val name: String)
struct Pair(val user: User, val tag: String)
class Box(val pair: Pair)

fn make(): Box { Box(Pair(User("持" + "有"), "A\0" + "B")) }

fn main(): Unit {
    val live = make()
    var count = 0
    while count < 12000 {
        Box(Pair(User("临" + "时"), "tag"))
        count += 1
    }
    gcCollect()
    print(live.pair.user.name)
    print(live.pair.tag)
    if live.pair.tag == "A\0B" { print(1) }
    if live.pair.tag != "A" { print(2) }
}
