let socket = null;

function openSignallingChannel() {
    let new_socket = new WebSocket("ws://18.236.77.108:9002");
    new_socket.onopen = () => {
        socket = new_socket;
        let button = document.getElementById("callButton");
        button.textContent = "Connected!";
        button.disabled = true;

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

function callReceiver() {
    openSignallingChannel();
}

async function makeCall() {
    const configuration = {iceServers: [
        {
            urls: "turn:54.200.166.206:3478?transport=tcp",
            username: "user",
            credential: "root"
        }
    ]};
    
    const peerConnection = new RTCPeerConnection(configuration);
    let stream = await navigator.mediaDevices.getDisplayMedia({
        video: {
            cursor: "always"
        },
        audio: false
    });
    
    stream.getTracks().forEach(track => { peerConnection.addTrack(track); });
    console.log("Added track");

    peerConnection.onconnectionstatechange = event => {
        console.log(peerConnection.connectionState);
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
