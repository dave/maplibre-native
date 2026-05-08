import MapLibre
import SwiftUI
import UIKit

// #-example-code(PMTilesStyleURL)
class PMTilesStyleURL: UIViewController {
    override func viewDidLoad() {
        super.viewDidLoad()
        let mapView = MLNMapView(
            frame: view.bounds,
            styleURL: URL(string: "https://demotiles.maplibre.org/pmtiles/vector/style.json")!
        )
        mapView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        view.addSubview(mapView)
    }
}

// #-end-example-code

// #-example-code(PMTilesAddSource)
class PMTilesAddSource: UIViewController, MLNMapViewDelegate {
    var mapView: MLNMapView!

    override func viewDidLoad() {
        super.viewDidLoad()
        mapView = MLNMapView(frame: view.bounds)
        mapView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        mapView.delegate = self
        view.addSubview(mapView)
    }

    func mapView(_: MLNMapView, didFinishLoading style: MLNStyle) {
        let url = URL(string: "pmtiles://https://demotiles.maplibre.org/pmtiles/vector/world.pmtiles")!
        let source = MLNVectorTileSource(identifier: "world", configurationURL: url)
        style.addSource(source)
        let layer = MLNFillStyleLayer(identifier: "countries", source: source)
        layer.sourceLayerIdentifier = "countries"
        layer.fillColor = NSExpression(forConstantValue: UIColor(red: 0.67, green: 0.83, blue: 0.87, alpha: 1))
        style.addLayer(layer)
    }
}

// #-end-example-code

// #-example-code(PMTilesLocalFile)
class PMTilesLocalFile: UIViewController, MLNMapViewDelegate {
    var mapView: MLNMapView!

    override func viewDidLoad() {
        super.viewDidLoad()
        mapView = MLNMapView(frame: view.bounds)
        mapView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        mapView.delegate = self
        view.addSubview(mapView)
    }

    func mapView(_: MLNMapView, didFinishLoading style: MLNStyle) {
        guard let fileURL = Bundle.main.url(forResource: "world", withExtension: "pmtiles") else { return }
        let url = URL(string: "pmtiles://\(fileURL.absoluteString)")!
        let source = MLNVectorTileSource(identifier: "world", configurationURL: url)
        style.addSource(source)
        let layer = MLNFillStyleLayer(identifier: "countries", source: source)
        layer.sourceLayerIdentifier = "countries"
        layer.fillColor = NSExpression(forConstantValue: UIColor(red: 0.67, green: 0.83, blue: 0.87, alpha: 1))
        style.addLayer(layer)
    }
}

// #-end-example-code

struct PMTilesStyleURLRepresentable: UIViewControllerRepresentable {
    func makeUIViewController(context _: Context) -> PMTilesStyleURL { PMTilesStyleURL() }
    func updateUIViewController(_: PMTilesStyleURL, context _: Context) {}
}

struct PMTilesAddSourceRepresentable: UIViewControllerRepresentable {
    func makeUIViewController(context _: Context) -> PMTilesAddSource { PMTilesAddSource() }
    func updateUIViewController(_: PMTilesAddSource, context _: Context) {}
}

struct PMTilesLocalFileRepresentable: UIViewControllerRepresentable {
    func makeUIViewController(context _: Context) -> PMTilesLocalFile { PMTilesLocalFile() }
    func updateUIViewController(_: PMTilesLocalFile, context _: Context) {}
}
