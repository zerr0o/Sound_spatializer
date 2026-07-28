import { ContactShadows, Grid, Html, Line, OrbitControls, RoundedBox } from '@react-three/drei';
import { Canvas, useFrame } from '@react-three/fiber';
import { Component, type ErrorInfo, type PropsWithChildren, useMemo, useRef } from 'react';
import * as THREE from 'three';
import { Box, Rotate3D } from 'lucide-react';
import { isSpeakerRouted, SPEAKER_COLORS } from '../../lib/speaker-layout';
import type {
  Channel,
  DisplayRuntimeInfo,
  HeadPoseSampleV1,
  InputLayout,
  ListenerConfig,
  RoomConfig,
  SpatialInputMode,
  SpeakerConfig,
  SpeakerSet,
  WindowAudioSourceInfo,
} from '../../types/contracts';

function Speaker({ speaker, selected, routed, onSelect }: { speaker: SpeakerConfig; selected: boolean; routed: boolean; onSelect: () => void }) {
  const color = SPEAKER_COLORS[speaker.channel];
  return (
    <group position={[speaker.position.x, speaker.position.y, -speaker.position.z]} onClick={(event) => { event.stopPropagation(); onSelect(); }}>
      {selected && (
        <mesh rotation={[-Math.PI / 2, 0, 0]} position={[0, -0.56, 0]}>
          <ringGeometry args={[0.42, 0.48, 48]} />
          <meshBasicMaterial color={color} transparent opacity={0.75} />
        </mesh>
      )}
      <RoundedBox args={[0.45, 0.84, 0.38]} radius={0.055} smoothness={4} castShadow>
        <meshStandardMaterial color="#161e27" roughness={0.38} metalness={0.35} transparent opacity={routed ? 1 : 0.42} />
      </RoundedBox>
      <mesh position={[0, 0.2, 0.197]}>
        <circleGeometry args={[0.104, 40]} />
        <meshStandardMaterial color="#070b10" roughness={0.55} transparent opacity={routed ? 1 : 0.42} />
      </mesh>
      <mesh position={[0, -0.15, 0.2]}>
        <circleGeometry args={[0.16, 40]} />
        <meshStandardMaterial color="#05080b" roughness={0.45} transparent opacity={routed ? 1 : 0.42} />
      </mesh>
      <mesh position={[0, -0.15, 0.205]}>
        <ringGeometry args={[0.11, 0.148, 40]} />
        <meshStandardMaterial color={color} emissive={color} emissiveIntensity={routed ? selected ? 0.75 : 0.24 : 0.03} transparent opacity={routed ? 1 : 0.34} />
      </mesh>
      <Html center position={[0, 0.69, 0]} distanceFactor={7} style={{ pointerEvents: 'none' }}>
        <span className={`object-label speaker-${speaker.channel.toLowerCase()}`}>{speaker.channel}</span>
      </Html>
    </group>
  );
}

function Listener({ pose, listener }: { pose: HeadPoseSampleV1 | null; listener: ListenerConfig }) {
  const head = useRef<THREE.Group>(null);
  const target = useMemo(() => new THREE.Quaternion(), []);
  useFrame((_, delta) => {
    if (!head.current) return;
    const q = pose?.quaternion ?? { x: 0, y: 0, z: 0, w: 1 };
    // Le monde audio utilise +Z vers l’avant, tandis que l’avant Three.js est -Z.
    // La réflexion de base conjugue la rotation en (-x, -y, z, w).
    target.set(-q.x, -q.y, q.z, q.w);
    head.current.quaternion.slerp(target, 1 - Math.exp(-delta * 18));
  });

  return (
    <group position={[listener.position.x, listener.position.y, -listener.position.z]}>
      <group ref={head}>
        <mesh castShadow>
          <sphereGeometry args={[0.22, 32, 24]} />
          <meshStandardMaterial color="#d4beb0" roughness={0.7} />
        </mesh>
        <mesh position={[0, 0, -0.203]} rotation={[Math.PI / 2, 0, 0]}>
          <coneGeometry args={[0.055, 0.14, 20]} />
          <meshStandardMaterial color="#b99f91" />
        </mesh>
        <mesh position={[-0.14, 0, 0]} rotation={[0, 0, Math.PI / 2]}>
          <torusGeometry args={[0.18, 0.035, 12, 48, Math.PI]} />
          <meshStandardMaterial color="#101820" metalness={0.55} roughness={0.35} />
        </mesh>
        <mesh position={[0.14, 0, 0]} rotation={[0, 0, -Math.PI / 2]}>
          <torusGeometry args={[0.18, 0.035, 12, 48, Math.PI]} />
          <meshStandardMaterial color="#101820" metalness={0.55} roughness={0.35} />
        </mesh>
        <mesh position={[0, 0, -0.47]} rotation={[-Math.PI / 2, 0, 0]}>
          <coneGeometry args={[0.025, 0.45, 16]} />
          <meshBasicMaterial color="#d6f8f2" transparent opacity={0.75} />
        </mesh>
      </group>
      <mesh position={[0, -0.43, 0]} castShadow>
        <cylinderGeometry args={[0.34, 0.45, 0.55, 40]} />
        <meshStandardMaterial color="#162530" roughness={0.62} />
      </mesh>
      <mesh position={[0, -1.15, 0]} rotation={[-Math.PI / 2, 0, 0]}>
        <ringGeometry args={[0.33, 0.345, 64]} />
        <meshBasicMaterial color="#d8f8f1" transparent opacity={0.22} />
      </mesh>
      <Html center position={[0, 0.5, 0]} distanceFactor={7} style={{ pointerEvents: 'none' }}>
        <span className="object-label listener-label">VOUS</span>
      </Html>
    </group>
  );
}

function SoundPath({ speaker, listener }: { speaker: SpeakerConfig; listener: ListenerConfig }) {
  const color = SPEAKER_COLORS[speaker.channel];
  return (
    <>
      <Line
        points={[
          [speaker.position.x, speaker.position.y, -speaker.position.z],
          [listener.position.x, listener.position.y, -listener.position.z],
        ]}
        color={color}
        lineWidth={1}
        transparent
        opacity={0.38}
        dashed
        dashScale={1.5}
        dashSize={0.1}
        gapSize={0.08}
      />
      {[0.22, 0.45, 0.68].map((progress) => (
        <Pulse key={progress} speaker={speaker} listener={listener} progress={progress} color={color} />
      ))}
    </>
  );
}

function Pulse({ speaker, listener, progress, color }: { speaker: SpeakerConfig; listener: ListenerConfig; progress: number; color: string }) {
  const mesh = useRef<THREE.Mesh>(null);
  useFrame(({ clock }) => {
    if (!mesh.current) return;
    const t = (progress + clock.elapsedTime * 0.17) % 1;
    mesh.current.position.set(
      THREE.MathUtils.lerp(speaker.position.x, listener.position.x, t),
      THREE.MathUtils.lerp(speaker.position.y, listener.position.y, t),
      THREE.MathUtils.lerp(-speaker.position.z, -listener.position.z, t),
    );
    mesh.current.scale.setScalar(0.6 + t * 0.8);
    const material = mesh.current.material as THREE.MeshBasicMaterial;
    material.opacity = Math.sin(t * Math.PI) * 0.34;
  });
  return (
    <mesh ref={mesh} rotation={[Math.PI / 2, 0, 0]}>
      <ringGeometry args={[0.035, 0.048, 24]} />
      <meshBasicMaterial color={color} transparent opacity={0.2} depthWrite={false} />
    </mesh>
  );
}

function RoomOutline({ room }: { room: RoomConfig }) {
  const { width, length, height } = room.dimensions;
  return (
    <group position={[0, height / 2, 0]}>
      <lineSegments>
        <edgesGeometry args={[new THREE.BoxGeometry(width, height, length)]} />
        <lineBasicMaterial color="#617382" transparent opacity={room.enabled ? 0.25 : 0.12} />
      </lineSegments>
      {room.enabled && (
        <mesh position={[0, -height / 2 + 0.015, 0]} rotation={[-Math.PI / 2, 0, 0]} receiveShadow>
          <planeGeometry args={[width, length]} />
          <meshStandardMaterial color="#101b22" roughness={0.9} transparent opacity={0.45} />
        </mesh>
      )}
    </group>
  );
}

function DisplayPlane({ display }: { display: DisplayRuntimeInfo }) {
  const orientation: [number, number, number, number] = [
    -display.orientation.x,
    -display.orientation.y,
    display.orientation.z,
    display.orientation.w,
  ];
  return (
    <group
      position={[display.center.x, display.center.y, -display.center.z]}
      quaternion={orientation}
    >
      <RoundedBox args={[display.widthM + 0.045, display.heightM + 0.045, 0.035]} radius={0.025} smoothness={3}>
        <meshStandardMaterial color="#15242c" metalness={0.45} roughness={0.36} />
      </RoundedBox>
      <mesh position={[0, 0, -0.021]}>
        <planeGeometry args={[display.widthM, display.heightM]} />
        <meshStandardMaterial
          color={display.isPrimary ? '#163b3b' : '#172b3d'}
          emissive={display.isPrimary ? '#55d9c5' : '#668dff'}
          emissiveIntensity={0.11}
          roughness={0.68}
        />
      </mesh>
      <Html center position={[0, display.heightM / 2 + 0.13, 0]} distanceFactor={6} style={{ pointerEvents: 'none' }}>
        <span className="object-label display-label">
          {display.name}{display.isPrimary ? ' · PRINCIPAL' : ''}
        </span>
      </Html>
    </group>
  );
}

function WindowSource({
  source,
  listener,
}: {
  source: WindowAudioSourceInfo;
  listener: ListenerConfig;
}) {
  const channels = [
    { key: 'L', color: SPEAKER_COLORS.L, position: source.leftPosition },
    { key: 'R', color: SPEAKER_COLORS.R, position: source.rightPosition },
  ] as const;
  return (
    <group>
      {channels.map((channel) => (
        <group key={channel.key}>
          <Line
            points={[
              [channel.position.x, channel.position.y, -channel.position.z],
              [listener.position.x, listener.position.y, -listener.position.z],
            ]}
            color={channel.color}
            lineWidth={1}
            transparent
            opacity={source.active ? 0.45 : 0.12}
            dashed
            dashScale={1.4}
            dashSize={0.08}
            gapSize={0.07}
          />
          <mesh position={[channel.position.x, channel.position.y, -channel.position.z]}>
            <sphereGeometry args={[0.075, 20, 16]} />
            <meshStandardMaterial
              color={channel.color}
              emissive={channel.color}
              emissiveIntensity={source.active ? 0.72 : 0.08}
              transparent
              opacity={source.active ? 1 : 0.38}
            />
          </mesh>
        </group>
      ))}
      <Html
        center
        position={[
          (source.leftPosition.x + source.rightPosition.x) / 2,
          Math.max(source.leftPosition.y, source.rightPosition.y) + 0.16,
          -(source.leftPosition.z + source.rightPosition.z) / 2,
        ]}
        distanceFactor={6}
        style={{ pointerEvents: 'none' }}
      >
        <span className="object-label source-label">
          {source.applicationName || source.windowTitle || `PID ${source.processId}`} · L/R
        </span>
      </Html>
    </group>
  );
}

interface SceneProps {
  speakers: SpeakerSet;
  listener: ListenerConfig;
  room: RoomConfig;
  pose: HeadPoseSampleV1 | null;
  inputLayout: InputLayout;
  spatialInputMode: SpatialInputMode;
  displays: DisplayRuntimeInfo[];
  windowSources: WindowAudioSourceInfo[];
  selectedSpeaker: Channel;
  onSelectSpeaker: (id: Channel) => void;
}

export function SpatialScene(props: SceneProps) {
  return (
    <SceneErrorBoundary>
      <Canvas
        shadows
        dpr={[1, 1.6]}
        camera={{ position: [4.4, 4.2, 5.5], fov: 40, near: 0.1, far: 60 }}
        gl={{ antialias: true, powerPreference: 'high-performance', alpha: true }}
      >
        <color attach="background" args={['#0b1218']} />
        <fog attach="fog" args={['#0b1218', 9, 17]} />
        <ambientLight intensity={0.75} />
        <directionalLight position={[2, 7, 4]} intensity={2.1} color="#dff8ff" castShadow shadow-mapSize={[1024, 1024]} />
        <pointLight position={[-4, 2.5, -2]} intensity={6} distance={7} color="#57dac4" />
        <pointLight position={[4, 2.5, -2]} intensity={5} distance={7} color="#668dff" />
        {props.spatialInputMode === 'endpoint-mix' && <RoomOutline room={props.room} />}
        <Grid
          position={[0, 0.015, -1.8]}
          args={[12, 12]}
          cellSize={0.25}
          cellThickness={0.45}
          cellColor="#21313b"
          sectionSize={1}
          sectionThickness={0.65}
          sectionColor="#324853"
          fadeDistance={10}
          fadeStrength={1.2}
          infiniteGrid
        />
        {props.spatialInputMode === 'endpoint-mix'
          ? props.speakers.map((speaker) => (
            <group key={speaker.id}>
              <Speaker speaker={speaker} selected={props.selectedSpeaker === speaker.id} routed={isSpeakerRouted(props.inputLayout, speaker.id)} onSelect={() => props.onSelectSpeaker(speaker.id)} />
              {isSpeakerRouted(props.inputLayout, speaker.id) && !speaker.muted && <SoundPath speaker={speaker} listener={props.listener} />}
            </group>
          ))
          : (
            <>
              {props.displays.map((display) => <DisplayPlane key={display.displayId} display={display} />)}
              {props.windowSources.map((source) => <WindowSource key={source.sourceId} source={source} listener={props.listener} />)}
            </>
          )}
        <Listener pose={props.pose} listener={props.listener} />
        <ContactShadows position={[0, 0.01, 0]} opacity={0.36} scale={10} blur={2.4} far={4} />
        <OrbitControls
          makeDefault
          target={[0, 0.9, -1.1]}
          minDistance={4}
          maxDistance={10}
          minPolarAngle={0.25}
          maxPolarAngle={Math.PI / 2.12}
          enableDamping
          dampingFactor={0.07}
        />
      </Canvas>
    </SceneErrorBoundary>
  );
}

class SceneErrorBoundary extends Component<PropsWithChildren, { error: boolean }> {
  state = { error: false };
  static getDerivedStateFromError() {
    return { error: true };
  }
  componentDidCatch(error: Error, info: ErrorInfo) {
    console.warn('WebGL scene unavailable', error, info);
  }
  render() {
    if (this.state.error) {
      return (
        <div className="scene-fallback">
          <Box size={32} />
          <strong>Aperçu 3D indisponible</strong>
          <p>La configuration reste accessible dans le panneau latéral.</p>
          <Rotate3D size={16} />
        </div>
      );
    }
    return this.props.children;
  }
}
