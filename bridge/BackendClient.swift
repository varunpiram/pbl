import Foundation
final class BackendClient {
    private let connection: NSXPCConnection
    init(helperRequirement: String) {
        connection = NSXPCConnection(machServiceName: PBL_SERVICE, options: .privileged)
        connection.remoteObjectInterface = NSXPCInterface(with: PBLBackend.self)
        connection.setCodeSigningRequirement(helperRequirement)
        connection.activate()
    }
    deinit { connection.invalidate() }
    func request(_ input: Data, completion: @escaping (Result<Data, Error>) -> Void) {
        let finish: (Data?, String?) -> Void = { value, error in
            let result: Result<Data, Error> = error.map { .failure(NSError(domain: "pbl", code: 1, userInfo: [NSLocalizedDescriptionKey: $0])) } ?? .success(value ?? Data())
            DispatchQueue.main.async { completion(result) }
        }
        let proxy = connection.remoteObjectProxyWithErrorHandler { finish(nil, $0.localizedDescription) } as! PBLBackend
        proxy.request(input, reply: finish)
    }
}
