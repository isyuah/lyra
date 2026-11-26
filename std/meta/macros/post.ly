/// Declare a POST route to include in the current package's #mountRoutes call.
fn post(target: String, route: String): Syntax { httpRoute(target, route, "POST") }
