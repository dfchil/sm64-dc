// Records the HDMI capture's audio to a WAV file until SIGTERM. Every buffer the device delivers
// reaches the file; ffmpeg's avfoundation input keeps only the latest and loses about one in five.
//   dc_hdmi_audio out.wav [device name]
import AVFoundation
import Foundation

let out = CommandLine.arguments[1]
let name = CommandLine.arguments.count > 2 ? CommandLine.arguments[2] : "Live Gamer Ultra 2.1-Audio"
let found = AVCaptureDevice.DiscoverySession(deviceTypes: [.external, .microphone], mediaType: .audio,
                                             position: .unspecified).devices
guard let device = found.first(where: { $0.localizedName == name }) else {
    fputs("no audio device named \(name)\n", stderr)
    exit(1)
}

final class Finish: NSObject, AVCaptureFileOutputRecordingDelegate {
    func fileOutput(_ output: AVCaptureFileOutput, didFinishRecordingTo url: URL,
                    from connections: [AVCaptureConnection], error: Error?) {
        if let error { fputs("\(error)\n", stderr) }
        exit(0)
    }
}

let session = AVCaptureSession()
session.addInput(try AVCaptureDeviceInput(device: device))
let file = AVCaptureAudioFileOutput()
session.addOutput(file)
session.startRunning()
let finish = Finish()
try? FileManager.default.removeItem(atPath: out)
file.startRecording(to: URL(fileURLWithPath: out), outputFileType: .wav, recordingDelegate: finish)

signal(SIGTERM, SIG_IGN)
let term = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
term.setEventHandler { file.stopRecording() }
term.resume()
dispatchMain()
