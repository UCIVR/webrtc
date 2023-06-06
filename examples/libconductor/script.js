let socket = null;

function openSignallingChannel() {
    //let ip = "18.236.77.108";
    let ip = "localhost";
    let new_socket = new WebSocket(`ws://${ip}:${document.getElementById("port").value}`);
    new_socket.onopen = () => {
        socket = new_socket;
        disable();
        if (!socket) {
            alert("No signalling socket");
            return;
        }

        makeCall();
    };

    new_socket.onerror = (_, event) => {
        alert("Couldn't connect");
    }
}

function disable() {
    let button = document.getElementById("callButton");
    button.textContent = "Connected!";
    button.disabled = true;
}

function enable() {
    let button = document.getElementById("callButton");
    button.textContent = "Share Screen";
    button.disabled = false;
}

function callReceiver() {
    openSignallingChannel();
}

async function makeCall() {
    const configuration = {
        iceServers: [
            {
                urls: "turn:54.200.166.206:3478?transport=tcp",
                username: "user",
                credential: "root"
            }
        ]
    };

    const peerConnection = new RTCPeerConnection(configuration);

    let stream = null;
    if (document.getElementById("port").value == "9002") {
        console.log("Adding stream");
        stream = await navigator.mediaDevices.getDisplayMedia({
            video: {
                cursor: "always"
            },
            audio: false
        });

        stream.getTracks().forEach(track => { peerConnection.addTrack(track); });
        console.log("Added track");
    } else {
        console.log("Adding data channel");
        peerConnection.addTransceiver("video", { direction: "recvonly" })
        peerConnection.createDataChannel("dummyChannel");
    }

    peerConnection.ontrack = event => alert("TRACK");
    peerConnection.ondatachannel = event => alert("CHANNEL");
    peerConnection.onconnectionstatechange = event => {
        console.log(peerConnection.connectionState);
        if (peerConnection.connectionState == "disconnected") {
            socket.close();
            socket = null;
            enable();
            peerConnection.close();
            if (stream != null) {
                stream.getTracks().forEach(track => { track.stop(); });
            }
        }
    };

    socket.onmessage = async message => {
        message = JSON.parse(message.data)
        console.log(message);
        if (message.answer) {
            const remoteDesc = new RTCSessionDescription(message.answer);
            await peerConnection.setRemoteDescription(remoteDesc);
            console.log("set remote description")
        } else if (message.iceCandidate) {
            try {
                await peerConnection.addIceCandidate(message.iceCandidate);
                console.log("added candidate");
            } catch (e) {
                console.error('Error adding received ice candidate', e);
            }
        } else {
            console.warn(message);
        }
    };

    const offer = await peerConnection.createOffer();
    await peerConnection.setLocalDescription(offer);
    socket.send(JSON.stringify({ 'offer': offer }));

    peerConnection.onicecandidate = event => {
        if (event.candidate) {
            socket.send(JSON.stringify({ 'new-ice-candidate': event.candidate }));
        }
    };

    peerConnection.onicegatheringstatechange = _ => {
        console.log("[ICE] " + peerConnection.iceGatheringState);
    }

    peerConnection.oniceconnectionstatechange = _ => {
        console.log("[ICE Connection] " + peerConnection.iceConnectionState);
    }
}
