/// Declare a GET route to include in the current package's #mountRoutes call.
fn get(target: String, route: String): Syntax { httpRoute(target, route, "GET") }
