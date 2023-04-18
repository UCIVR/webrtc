let socket = null;

function openSignallingChannel() {
    let new_socket = new WebSocket("ws://localhost:9002");
    new_socket.onopen = () => {
        socket = new_socket;
        let button = document.getElementById("socketButton");
        button.textContent = "Connected!";
        button.disabled = true;
    };

    new_socket.onerror = (_, event) => {
        alert("Couldn't connect");
    }
}

function callReceiver() {
    if (!socket) {
        alert("No signalling socket");
        return;
    }

    makeCall();
}

async function makeCall() {
    const configuration = {'iceServers': [{'urls': 'stun:stun.l.google.com:19302'}]}
    const peerConnection = new RTCPeerConnection(configuration);
    let stream = await navigator.mediaDevices.getUserMedia({video: true})
    stream.getTracks().forEach(track => { peerConnection.addTrack(track); });
    console.log("Added track");

    peerConnection.onconnectionstatechange = event => {
        console.log(peerConnection.connectionState);
    };

    socket.onmessage = async message => {
        if (message.answer) {
            const remoteDesc = new RTCSessionDescription(message.answer);
            await peerConnection.setRemoteDescription(remoteDesc);
        } else if (message.iceCandidate) {
            try {
                await peerConnection.addIceCandidate(message.iceCandidate);
                console.log("added candidate");
            } catch (e) {
                console.error('Error adding received ice candidate', e);
            }
        }
    };

    const offer = await peerConnection.createOffer();
    await peerConnection.setLocalDescription(offer);
    socket.send(JSON.stringify({'offer': offer}));

    peerConnection.onicecandidate = event => {
        if (event.candidate) {
            socket.send(JSON.stringify({'new-ice-candidate': event.candidate}));
        }
    };

    peerConnection.onicegatheringstatechange = _ => {
        console.log("[ICE] " + peerConnection.iceGatheringState);
    }

    peerConnection.oniceconnectionstatechange = _ => {
        console.log("[ICE Connection] " + peerConnection.iceConnectionState);
    }
}
